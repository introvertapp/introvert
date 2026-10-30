#include "detection_model.h"
#include <QCoreApplication>
#include <QEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRunnable>
#include <algorithm>

namespace introvert::ui {
DetectionModel::DetectionModel(LibraryModel &library, SettingsStore settings, segments::SegmentStore segments,
    std::shared_ptr<detection::MediaDetectionRunner> runner, detection::DetectionService::Completion completion, QObject *parent)
    : QObject(parent), m_library(library), m_store(settings.databasePath()),
      m_service(m_store, std::move(segments), runner ? std::move(runner) : std::make_shared<detection::MediaDetectionEngine>(settings.databasePath(), library.m_cache),
        [&library](const auto &saved, const auto &cancel) { return library.m_catalog.config(saved, cancel); }, std::move(completion))
{
    m_pool.setMaxThreadCount(1); m_store.initialize(); connect(&library, &LibraryModel::changed, this, &DetectionModel::sync); sync();
}
DetectionModel::~DetectionModel() { close(); }
bool DetectionModel::sameConnection(const CatalogSelection &selection) const
{
    const auto &key=m_library.m_key;
    return key && key->saved.provider==selection.saved.provider && key->saved.remoteUrl==selection.saved.remoteUrl
        && key->saved.localRoot==selection.saved.localRoot;
}
bool DetectionModel::sameShow(const providers::ProviderIdentity &show) const
{
    if(!m_library.m_detail)return false;
    const auto &current=m_library.m_detail->show.identity;
    return current.provider==show.provider && current.itemId==show.itemId && current.serverId==show.serverId;
}
void DetectionModel::publish(const detection::DetectionUpdates &updates)
{
    // Publish the complete season in one GUI snapshot. Navigation can change
    // the view revision without changing the provider/server/show identity.
    for(auto it=updates.cbegin();it!=updates.cend();++it)if(m_library.episodeContext(it.key())) {
        m_library.m_segments.values[it.key()]=it.value().first;
        if(it.value().second)m_library.m_segments.edited.insert(it.key());else m_library.m_segments.edited.remove(it.key());
    }
    m_library.m_episodes=m_library.episodesForSeason(m_library.m_season);
}
void DetectionModel::sync()
{
    if (m_closed) return;
    bool applied=false;auto pending=std::exchange(m_pending,{});
    for(const auto &entry:pending) {
        if(entry.generation!=m_library.m_generation || !sameConnection(entry.selection))continue;
        if(sameShow(entry.show)){publish(entry.updates);applied=true;}
        else if(m_library.busy())m_pending.append(entry);
    }
    if(applied)emit m_library.changed();
    std::optional<QString> context;
    if (m_library.m_detail) {
        const auto &identity = m_library.m_detail->show.identity;
        context = providers::providerName(identity.provider) + '\n' + identity.itemId + '\n' + QString::number(m_library.season());
    }
    if (context != m_context) { m_message.clear(); m_displayJob.clear(); m_context = context; }
    try {
        const auto previous = m_status;
        m_status.clear(); for (const auto &value : m_store.rows()) {
            const auto row = value.toMap(); const auto key = row.value("provider").toString() + '\n' + row.value("show_id").toString() + '\n' + row.value("season").toString();
            if (context && *context == key) {
                const auto state = row.value("status").toString();
                const auto id = row.value("id").toString();
                const bool ongoing = state == "queued" || state == "running" || m_jobs.contains(id);
                // History remains durable, but terminal feedback belongs only
                // to the screen visit that observed the job running.
                if (ongoing) m_displayJob = id;
                else if (m_displayJob != id) break;
                m_status = row;
                if (m_jobs.contains(row.value("id").toString()) && state != "queued" && state != "running") {
                    // SQLite can finish before the queued GUI result delivery.
                    // Publish completion only after every episode has refreshed.
                    const bool sameJob = previous.value("id") == row.value("id");
                    m_status["status"] = "running";
                    m_status["current"] = sameJob ? previous.value("current") : QVariant(std::max(0, row.value("total").toInt() - 1));
                    m_status["message"] = sameJob ? previous.value("message") : QVariant("Scanning: saving results");
                }
                break;
            }
        }
        m_rows = m_store.queue();
    } catch (const DatabaseError &) {
        const QString message = "Cannot read detection jobs from app.db";
        if (m_message != message) emit errorOccurred(message);
        m_message = message;
    }
    emit changed();
}
void DetectionModel::start(const QVariantList &values)
{
    if (m_closed || !m_library.m_detail || !m_library.m_key) return;
    m_message.clear(); QStringList kinds;
    for (const auto &value : values) { const auto kind = value.toString(); if (!kinds.contains(kind)) kinds.append(kind); }
    if (kinds.empty() || std::any_of(kinds.begin(), kinds.end(), [](const auto &kind) { return kind != "intro" && kind != "credits"; })) { m_message = "Select Intro, Credits, or both"; emit changed(); return; }
    const auto detail = *m_library.m_detail; const auto season = m_library.season(); const auto saved = m_library.m_key->saved; QPair<QString, bool> created;
    auto editLock=m_library.m_editLocks.detect(detail.show,season);
    if(!editLock) {m_message="Detection is already running for this season";emit changed();return;}
    try { created = m_store.create(detail, season, kinds); }
    catch (const DatabaseError &) { m_message = "Cannot create a detection job in app.db"; emit errorOccurred(m_message); emit changed(); return; }
    if (created.second) {
        emit jobStarted(detail.show.title, season);
        const auto cancel = std::make_shared<std::atomic_bool>(false); m_jobs.insert(created.first, {cancel,editLock,*m_library.m_key,detail.show.identity}); const auto id = created.first;
        const auto gate=editLock.get();
        m_pool.start(QRunnable::create([this, id, detail, season, kinds, saved, cancel,gate] {
            detection::DetectionUpdates updates; QString error;
            try {gate->waitForWriters(cancel); updates = m_service.run(id, detail, season, kinds, saved, cancel, [this] { QMetaObject::invokeMethod(this, [this] { sync(); }, Qt::QueuedConnection); }); }
            catch(const providers::Cancelled &) {try {m_store.finish(id,"cancelled","Detection stopped");}catch (...) {error="Cannot update detection jobs in app.db";}}
            catch (...) { error = "Cannot update detection jobs in app.db"; }
            QMetaObject::invokeMethod(this, [this, id, updates, error] { finished(id, updates, error); }, Qt::QueuedConnection);
        }));
    }
    editLock.reset();emit m_library.changed();
    sync();
}
void DetectionModel::stop()
{
    const auto job = m_jobs.constFind(m_status.value("id").toString()); if (job == m_jobs.cend()) return;
    job->cancel->store(true); m_message = "Stopping detection"; emit changed();
}
void DetectionModel::finished(const QString &id, const detection::DetectionUpdates &updates, const QString &error)
{
    const auto found = m_jobs.find(id); if (found == m_jobs.end()) return; if (m_closed) return;
    if(sameConnection(found->selection)) {
        if(sameShow(found->show)) {
            publish(updates);
            // Open editors consume the new baseline while the season is still
            // locked; the following notification then unlocks editing.
            emit m_library.changed();
        }
        else if(m_library.busy() && m_library.m_show.value("id").toString()==found->show.itemId)
            m_pending.append({m_library.m_generation,found->selection,found->show,updates});
    }
    m_jobs.remove(id);
    emit m_library.changed();
    m_message = error; sync();
    if (!error.isEmpty()) { emit errorOccurred(error); return; }
    // Read the completed job by identity, including when its show is no longer
    // open. Do not infer completion from the current screen or replay history.
    try {
        for (const auto &value : m_store.rows()) {
            const auto row = value.toMap(); if (row.value("id").toString() != id) continue;
            const auto result = QJsonDocument::fromJson(row.value("result").toString().toUtf8()).object().toVariantMap();
            const auto diagnostics = QJsonDocument::fromJson(row.value("diagnostics").toString().toUtf8()).object().toVariantMap();
            const auto failures = diagnostics.value("failures").toList();
            QString message = row.value("message").toString();
            if (row.value("status") == "complete") {
                const auto count = result.value("updated").toMap().size();
                message = count == 0 ? "No new timestamps were saved." : QString("Timestamps saved for %1 %2.").arg(count).arg(count == 1 ? "episode" : "episodes");
                if (!failures.isEmpty()) message += " Some media could not be analyzed; review the season's results.";
                else if (!diagnostics.value("matching_warnings").toList().isEmpty()) message += " Some comparisons had limited evidence.";
            }
            emit jobFinished(row.value("title").toString(), row.value("season").toInt(), row.value("status").toString(), message);
            for (const auto &value : failures) {
                const auto failure = value.toMap();
                auto codes = failure.value("codes").toStringList().join(", "); codes.replace('_', ' ');
                const auto stage = failure.value("stage").toString();
                emit diagnosticMessage("warning", QString("%1, season %2: %3 could not finish. %4")
                    .arg(row.value("title").toString()).arg(row.value("season").toInt()).arg(stage.isEmpty() ? "A media comparison" : stage, codes));
            }
            const auto webhook = result.value("webhook").toMap();
            if (webhook.value("attempted").toBool()) {
                if (webhook.value("success").toBool()) emit diagnosticMessage("info", "Detection completion webhook delivered.");
                else emit errorOccurred("Detection finished, but its webhook notification could not be delivered. Check Settings → Notifications.");
            }
            break;
        }
    } catch (const DatabaseError &) { emit errorOccurred("Cannot read the completed detection job from app.db."); }
}
void DetectionModel::close()
{
    if (m_closed) return; m_closed = true; for (const auto &job : m_jobs) job.cancel->store(true);
    disconnect(&m_library, nullptr, this, nullptr); m_pool.waitForDone(); QCoreApplication::sendPostedEvents(this, QEvent::MetaCall); m_jobs.clear();m_pending.clear();
    emit m_library.changed();
}
} // namespace introvert::ui
