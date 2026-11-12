#include "detection_model.h"
#include "core/queue_order.h"
#include "detection/algorithm_client.h"
#include <QCoreApplication>
#include <QEvent>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
#include <QUuid>
#include <QRunnable>
#include <algorithm>
#include <cmath>

namespace introvert::ui {
namespace {
QString showKey(const CatalogSelection &selection, const providers::ProviderIdentity &show)
{
    const auto &saved = selection.saved;
    return QString::fromUtf8(QJsonDocument(QJsonArray{saved.provider, saved.remoteUrl, saved.localRoot,
        show.itemId, show.serverId.value_or(QString())}).toJson(QJsonDocument::Compact));
}
}

DetectionModel::DetectionModel(LibraryModel &library, SettingsStore settings, segments::SegmentStore segments,
    std::shared_ptr<detection::MediaDetectionRunner> runner, detection::DetectionService::Completion completion, QObject *parent, QString algorithmRoot)
    : QObject(parent), m_library(library), m_store(settings.databasePath()), m_requiresAlgorithm(!runner), m_algorithmRoot(std::move(algorithmRoot)),
      m_service(m_store, std::move(segments), runner ? std::move(runner) : std::make_shared<detection::MediaDetectionEngine>(settings.databasePath(), library.m_cache,QString(),QString(),QVariantMap{},m_algorithmRoot),
        [&library](const auto &saved, const auto &cancel) { return library.m_catalog.config(saved, cancel); }, std::move(completion))
{
    m_scanClock.setInterval(1000);
    connect(&m_scanClock, &QTimer::timeout, this, [this] {
        const auto previous = m_scan.value("estimatedSeconds"); updateScanEstimate();
        if (previous != m_scan.value("estimatedSeconds")) emit changed();
    });
    m_discoveryPool.setMaxThreadCount(1);
    m_pool.setMaxThreadCount(1); m_store.initialize(); connect(&library, &LibraryModel::changed, this, &DetectionModel::sync); sync();
    if(m_requiresAlgorithm) withAlgorithm({});
}
DetectionModel::~DetectionModel() { close(); }
bool DetectionModel::sameConnection(const CatalogSelection &selection) const
{
    const auto &key=m_library.m_key;
    return key && key->saved.provider==selection.saved.provider && key->saved.remoteUrl==selection.saved.remoteUrl
        && key->saved.localPaths("tv")==selection.saved.localPaths("tv");
}
bool DetectionModel::sameShow(const providers::ProviderIdentity &show) const
{
    if(!m_library.m_detail)return false;
    const auto &current=m_library.m_detail->show.identity;
    return current.provider==show.provider && current.itemId==show.itemId && current.serverId==show.serverId;
}
QVariantList DetectionModel::activeSeasons() const
{
    QVariantList seasons;
    for (const auto &job : m_jobs) if (sameConnection(job.selection) && sameShow(job.show) && !seasons.contains(job.season)) seasons.append(job.season);
    return seasons;
}
void DetectionModel::publish(const detection::DetectionUpdates &updates)
{
    if (!m_library.m_detail) return;
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
        context = (m_library.m_key ? showKey(*m_library.m_key, identity) : QString()) + '\n' + QString::number(m_library.season());
    }
    if (context != m_context) { m_message.clear(); m_displayJob.clear(); m_diagnosticsJob.clear(); m_diagnostics.clear(); m_context = context; }
    QString visit;
    if (m_library.m_detail && m_library.m_key) visit = showKey(*m_library.m_key, m_library.m_detail->show.identity);
    if (visit != m_showVisit) { m_showVisit = visit; m_seasonJobs.clear(); m_showDetectionObserved = false; }
    const auto scope = m_library.m_detail && m_library.m_key ? providers::providerScope(m_library.m_detail->show.identity,
        m_library.connectionAddress()) : QString();
    m_seasonProgress.clear(); m_showDetection.clear();
    try {
        const auto rows = m_store.summaries();
        QSet<int> seen;
        for (const auto &value : rows) {
            const auto row = value.toMap(); const auto id = row.value("id").toString(); const int season = row.value("season").toInt();
            if (visit.isEmpty() || seen.contains(season) || !m_library.m_detail
                || row.value("scope").toString() != scope
                || row.value("provider").toString() != providers::providerName(m_library.m_detail->show.identity.provider)
                || row.value("show_id") != m_library.m_detail->show.identity.itemId) continue;
            const auto job = m_jobs.constFind(id);
            if (job != m_jobs.cend() && sameConnection(job->selection) && sameShow(job->show)) m_seasonJobs[season] = id;
            if (m_seasonJobs.value(season) != id) continue;
            seen.insert(season);
            auto state = row.value("status").toString();
            double progress = std::clamp(row.value("current").toDouble() / std::max(1., row.value("total").toDouble()), 0., 1.);
            if (job != m_jobs.cend()) { if (state != "queued") state = "running"; progress = std::min(.999, progress); }
            m_seasonProgress[QString::number(season)] = QVariantMap{{"id", id}, {"status", state}, {"progress", state == "complete" ? 1. : progress}};
        }
        // The overall control follows queued work, independently of temporary
        // season selection and visit-scoped completion colors.
        const auto group = m_showJobs.value(visit);
        if (!visit.isEmpty() && !group.isEmpty()) {
            bool active = false, complete = true; double progress = 0.; int count = 0;
            QSet<int> seasons;
            for (const auto &value : rows) {
                const auto row = value.toMap(); const auto id = row.value("id").toString();
                if (!group.contains(id)) continue;
                ++count; seasons.insert(row.value("season").toInt());
                const bool pending = m_jobs.contains(id); const auto state = row.value("status").toString();
                active |= pending; complete &= !pending && state == "complete";
                if (!pending && (state == "complete" || state == "failed" || state == "cancelled")) progress += 1.;
                else progress += std::clamp(row.value("current").toDouble() / std::max(1., row.value("total").toDouble()), 0., .999);
            }
            if (active) m_showDetectionObserved = true;
            if (active || m_showDetectionObserved) {
                auto numbers = seasons.values(); std::sort(numbers.begin(), numbers.end()); QVariantList values;
                for (int number : numbers) values.append(number);
                m_showDetection = {{"active", active}, {"seasons", values},
                    {"progress", active ? std::min(.999, progress / group.size()) : complete && count == group.size() ? 1. : -1.}};
            }
        }
        const auto previous = m_status;
        m_status.clear(); for (const auto &value : rows) {
            const auto row = value.toMap();
            if (context && m_library.m_detail && row.value("scope").toString() == scope
                && row.value("provider").toString() == providers::providerName(m_library.m_detail->show.identity.provider)
                && row.value("show_id").toString() == m_library.m_detail->show.identity.itemId && row.value("season").toInt() == m_library.season()) {
                const auto state = row.value("status").toString();
                const auto id = row.value("id").toString();
                const bool ongoing = state == "queued" || state == "running" || m_jobs.contains(id);
                // History remains durable, but terminal feedback belongs only
                // to the screen visit that observed the job running.
                if (ongoing) m_displayJob = id;
                else if (m_displayJob != id) break;
                m_status = row;
                // Preserve the completed status contract using the one visible
                // job's worker-loaded string, never a history-wide SQL read.
                m_status["diagnostics"] = id == m_diagnosticsJob ? m_diagnostics : QString("{}");
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
        m_activeShowIds.clear();
        double seasonProgress = m_scanSeasonsDone;
        for (const auto &value : rows) {
            const auto row = value.toMap(); const auto id = row.value("id").toString();
            if (row.value("status") == "running" && m_jobs.contains(id)) {
                const auto &job = m_jobs[id];
                if (sameConnection(job.selection) && !m_activeShowIds.contains(job.show.itemId)) m_activeShowIds.append(job.show.itemId);
                if (libraryScanActive() && m_scanWaiting.contains(id)) { m_scan["currentSeason"] = job.season; m_scan["message"] = row.value("message"); }
            }
            if (m_scanWaiting.contains(id)) seasonProgress += std::clamp(row.value("current").toDouble() / std::max(1., row.value("total").toDouble()), 0., .999);
        }
        if (libraryScanActive() && !m_scanShows.isEmpty())
            m_scan["progress"] = std::max(m_scan.value("progress").toDouble(), std::min(.999, (m_scanIndex + (m_scanSeasons ? seasonProgress / m_scanSeasons : 0.)) / m_scanShows.size()));
        updateScanEstimate();
        m_rows.clear();
        for (const auto &value : detection::DetectionJobStore::queue(rows))
            if (!libraryScanActive() || !m_scanJobs.contains(value.toMap().value("jobId").toString())) m_rows.append(value);
        if (libraryScanActive()) m_rows.append(QVariantMap{{"type", "Detection"}, {"endpoint", ""}, {"show", "Full library"},
            {"episode", m_scan.value("currentShow")}, {"code", ""}, {"date", m_scan.value("date")}, {"status", "Detecting"},
            {"showId", ""}, {"providerId", m_scanSelection->saved.provider}, {"jobId", m_scan.value("id")},
            {"progressCurrent", int(m_scan.value("progress").toDouble() * 1000)}, {"progressTotal", 1000}});
    } catch (const DatabaseError &) {
        const QString message = "Cannot read detection jobs from app.db";
        if (m_message != message) emit errorOccurred(message);
        m_message = message;
    }
    emit changed();
}
void DetectionModel::scheduleSync()
{
    // A burst of worker progress needs one fresh snapshot, not a backlog of
    // identical database reads queued ahead of completion and user input.
    if (m_syncPending.exchange(true)) return;
    QMetaObject::invokeMethod(this, [this] { m_syncPending = false; sync(); }, Qt::QueuedConnection);
}
void DetectionModel::prepareShowDetection(const CatalogSelection &selection, const providers::ProviderIdentity &show)
{
    auto &group = m_showJobs[showKey(selection, show)];
    if (std::none_of(group.cbegin(), group.cend(), [this](const QString &id) { return m_jobs.contains(id); })) group.clear();
}
void DetectionModel::updateScanEstimate()
{
    qint64 seconds = -1;
    const double progress = m_scan.value("progress").toDouble();
    // Wait for a completed season/show so a short metadata phase cannot imply a misleading ETA.
    if (libraryScanActive() && m_scanElapsed.isValid() && m_scanElapsed.elapsed() >= 10000
        && (m_scanIndex > 0 || m_scanSeasonsDone > 0) && progress > 0. && !m_scanCancel->load())
        seconds = qint64(std::ceil(m_scanElapsed.elapsed() / 1000. * (1. - progress) / progress));
    m_scan["estimatedSeconds"] = seconds;
}
QStringList DetectionModel::validatedKinds(const QVariantList &values)
{
    QStringList kinds;
    for (const auto &value : values) { const auto kind = value.toString(); if (!kinds.contains(kind)) kinds.append(kind); }
    if (kinds.empty() || std::any_of(kinds.begin(), kinds.end(), [](const auto &kind) { return kind != "intro" && kind != "credits"; })) {
        m_message = "Select Intro, Credits, or both"; emit changed(); return {};
    }
    return kinds;
}
void DetectionModel::start(const QVariantList &values)
{
    startSeasons({m_library.season()}, values);
}
void DetectionModel::startSeasons(const QVariantList &seasons, const QVariantList &values)
{
    if (m_closed || !m_library.m_detail || !m_library.m_key) return;
    withAlgorithm([this,seasons,values] { startSeasonsReady(seasons,values); });
}
void DetectionModel::withAlgorithm(std::function<void()> ready)
{
    if (!m_requiresAlgorithm) { if(ready)ready(); return; }
    if (m_closed) return;
    if(!m_verifiedAlgorithmLibrary.isEmpty() && QFileInfo(m_verifiedAlgorithmLibrary).isFile()) {if(ready)ready();return;}
    if(ready)m_algorithmRequest=AlgorithmRequest{m_library.m_generation,m_library.m_showGeneration,std::move(ready)};
    if(m_checkingAlgorithm)return;
    m_checkingAlgorithm = true;
    const auto database=m_store.databasePath(),root=m_algorithmRoot;
    m_discoveryPool.start(QRunnable::create([this,database,root] {
        QString library,error;
        try {
            if(const auto package=detection::AlgorithmPackages(database,root).available()) {
                detection::Algorithm algorithm(*package);algorithm.description();library=package->library;
            }
        } catch(const detection::DetectionError &) {}
        catch(...) {error="Cannot check detector availability. Check application storage and try again.";}
        QMetaObject::invokeMethod(this,[this,library,error] {
            m_checkingAlgorithm=false;
            if(m_closed)return;
            m_verifiedAlgorithmLibrary=library;
            const auto pending=std::move(m_algorithmRequest);m_algorithmRequest.reset();
            if(!pending || pending->generation!=m_library.m_generation || pending->showGeneration!=m_library.m_showGeneration)return;
            if(!error.isEmpty()) {emit errorOccurred(error);return;}
            if(library.isEmpty()) {
                emit detectorRequired("The detector is not installed or is unavailable. Open Settings → Application, click Check for Updates, then download the detector. You can start detection after installation completes.");
                return;
            }
            pending->ready();
        },Qt::QueuedConnection);
    }));
}
void DetectionModel::startSeasonsReady(const QVariantList &seasons, const QVariantList &values)
{
    if (m_closed || !m_library.m_detail || !m_library.m_key) return;
    m_message.clear(); const auto kinds = validatedKinds(values); if (kinds.isEmpty()) return;
    const auto detail = *m_library.m_detail; const auto selection = *m_library.m_key;
    prepareShowDetection(selection, detail.show.identity);
    QSet<int> selected;
    for (const auto &value : seasons) {
        bool ok = false; const int number = value.toInt(&ok);
        if (!ok || value.toDouble() != number || selected.contains(number)) continue;
        const auto found = std::find_if(detail.seasons.cbegin(), detail.seasons.cend(), [number](const auto &season) { return season.number == number; });
        if (found == detail.seasons.cend()) continue;
        selected.insert(number); enqueue(detail, number, kinds, selection);
    }
    emit m_library.changed(); sync();
}
QString DetectionModel::enqueue(const providers::ShowDetail &detail, int season, const QStringList &kinds, const CatalogSelection &selection)
{
    // Join an existing request rather than reserving the same season twice.
    for (auto it = m_jobs.cbegin(); it != m_jobs.cend(); ++it) {
        const auto &job = it.value();
        if (job.season == season && job.show.provider == detail.show.identity.provider && job.show.itemId == detail.show.identity.itemId
            && job.show.serverId == detail.show.identity.serverId && job.selection.saved.provider == selection.saved.provider
            && job.selection.saved.remoteUrl == selection.saved.remoteUrl && job.selection.saved.localPaths("tv") == selection.saved.localPaths("tv")) {
            m_showJobs[showKey(selection, detail.show.identity)].insert(it.key()); return it.key();
        }
    }
    auto editLock = m_library.m_editLocks.detect(detail.show, season, selection.address());
    if (!editLock) { m_message = "Detection is already running for this season"; return {}; }
    QPair<QString, bool> created;
    try { created = m_store.create(detail, season, kinds, providers::providerScope(detail.show.identity,
        detail.show.localRoot.value_or(selection.address()))); }
    catch (const DatabaseError &) { m_message = "Cannot create a detection job in app.db"; emit errorOccurred(m_message); return {}; }
    if (!created.second) return {};
    emit jobStarted(detail.show.title, season);
    const auto cancel = std::make_shared<std::atomic_bool>(false);
    m_jobs.insert(created.first, {cancel, editLock, selection, detail.show.identity, season, kinds});
    m_showJobs[showKey(selection, detail.show.identity)].insert(created.first);
    const auto id = created.first; const auto saved = selection.saved;
    dispatch(id, [this, id, detail, season, kinds, saved, cancel, editLock] {
        detection::DetectionUpdates updates; QString error;
        try { updates = m_service.run(id, detail, season, kinds, saved, cancel, [this] { scheduleSync(); }, editLock); }
        catch (const providers::Cancelled &) { try { m_store.finish(id,"cancelled","Detection stopped"); } catch (...) { error="Cannot update detection jobs in app.db"; } }
        catch (...) { error = "Cannot update detection jobs in app.db"; try { m_store.finish(id, "failed", error); } catch (...) {} }
        QVariantMap completed;
        try {
            // Read just this job and parse notification evidence on the worker.
            // Keep its shared JSON string for the visible status contract.
            completed = m_store.row(id);
            const auto diagnostics = QJsonDocument::fromJson(completed.value("diagnostics").toString().toUtf8()).object();
            completed["failures"] = diagnostics.value("failures").toArray().toVariantList();
            completed["matchingWarnings"] = !diagnostics.value("matching_warnings").toArray().isEmpty();
            completed["result"] = QJsonDocument::fromJson(completed.value("result").toString().toUtf8()).object().toVariantMap();
        } catch (...) { if (error.isEmpty()) error = "Cannot read the completed detection job from app.db."; }
        QMetaObject::invokeMethod(this, [this, id, updates, error, completed] { finished(id, updates, error, completed); }, Qt::QueuedConnection);
    });
    return id;
}
void DetectionModel::dispatch(const QString &id, std::function<void()> task)
{
    std::lock_guard<std::mutex> lock(m_taskMutex);
    m_tasks.append({id, std::move(task)});
    if (m_dispatching) return;
    m_dispatching = true;
    m_pool.start(QRunnable::create([this] {
        for (;;) {
            QStringList keys;
            {
                std::lock_guard<std::mutex> lock(m_taskMutex);
                if (m_tasks.isEmpty()) { m_dispatching = false; return; }
                for (const auto &entry : m_tasks) keys.append("d:" + entry.first);
            }
            int selected = 0;
            try {
                sql::Connection connection(m_store.databasePath()); sql::Transaction transaction(connection.db);
                const auto key = queueOrder::claim(connection.db, keys);
                if (!key.isEmpty()) selected = keys.indexOf(key);
                transaction.commit();
            } catch (...) {
                // A transient writer/storage failure must not silently start the
                // FIFO job in place of the saved head. Shutdown still drains the
                // cancelled tasks so their reservations and callbacks are joined.
                if (!m_dispatchClosing.load()) { scheduleSync(); QThread::msleep(100); continue; }
            }
            std::function<void()> task;
            {
                // Only this dispatcher removes tasks; GUI enqueues only append.
                // Never hold the GUI's mutex while waiting for SQLite writers.
                std::lock_guard<std::mutex> lock(m_taskMutex);
                task = std::move(m_tasks[selected].second); m_tasks.removeAt(selected);
            }
            scheduleSync();
            task();
        }
    }));
}

void DetectionModel::stopSeasons(const QVariantList &seasons)
{
    if (!m_library.m_detail || !m_library.m_key) return;
    for (const auto &job : m_jobs) if (sameConnection(job.selection) && sameShow(job.show) && seasons.contains(job.season)) job.cancel->store(true);
    m_message = "Stopping detection"; emit changed();
}
void DetectionModel::startLibraryScan(const QVariantList &values)
{
    if (m_closed || libraryScanActive()) return;
    withAlgorithm([this,values] { startLibraryScanReady(values); });
}
void DetectionModel::startLibraryScanReady(const QVariantList &values)
{
    if (m_closed || libraryScanActive()) return;
    const auto kinds = validatedKinds(values); if (kinds.isEmpty()) return;
    auto *media = qobject_cast<ProviderSettingsModel *>(m_library.m_settings.mediaSettings());
    media->flush(); if (media->dirty()) return;
    try { m_scanSelection = m_library.settingsKey(); }
    catch (...) { emit errorOccurred("Cannot read the media configuration for the library scan."); return; }
    if (!m_scanSelection) { emit errorOccurred("Configure a media provider in Settings → Media before scanning the library."); return; }
    if (!m_scanSelection->saved.importsTv()) {
        m_scanSelection.reset();
        emit errorOccurred("Enable TV show imports in Settings → Media before scanning the library.");
        return;
    }
    m_scanCancel = std::make_shared<std::atomic_bool>(false); m_scanKinds = kinds;
    m_scanShows.clear(); m_scanJobs.clear(); m_scanOwnedJobs.clear(); m_scanWaiting.clear(); m_scanFollowups.clear(); m_scanDetail.reset();
    m_scanIndex = m_scanSeasons = m_scanSeasonsDone = 0; m_scanPreparing = true;
    m_scan = {{"id", QUuid::createUuid().toString(QUuid::WithoutBraces)}, {"active", true}, {"status", "running"},
        {"message", "Loading the library…"}, {"totalShows", 0}, {"completedShows", 0}, {"remainingShows", 0},
        {"failedSeasons", 0}, {"failedShows", 0}, {"progress", 0.}, {"currentShow", ""}, {"currentSeason", 0},
        {"date", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
    const auto selection = *m_scanSelection; const auto cancel = m_scanCancel; const auto generation = ++m_scanGeneration;
    m_scanElapsed.start(); m_scanClock.start();
    emit diagnosticMessage("info", "Full-library detection started."); sync();
    m_discoveryPool.start(QRunnable::create([this, selection, cancel, generation] {
        QList<providers::ShowSummary> shows; QString error;
        try { shows = m_library.m_catalog.series(m_library.m_catalog.config(selection.saved, cancel), cancel); }
        catch (const providers::Cancelled &) {}
        catch (...) { error = "Cannot load the library for detection. Check Settings → Media and the server connection."; }
        QMetaObject::invokeMethod(this, [this, shows, error, generation] {
            if (m_closed || generation != m_scanGeneration) return;
            m_scanPreparing = false;
            if (m_scanCancel->load()) { finishScan("cancelled", "Library scan stopped."); return; }
            if (!error.isEmpty()) { finishScan("failed", error); emit errorOccurred(error); return; }
            m_scanShows = shows; m_scan["totalShows"] = shows.size(); m_scan["remainingShows"] = shows.size(); advanceScan();
        }, Qt::QueuedConnection);
    }));
}
void DetectionModel::advanceScan()
{
    if (m_closed || !libraryScanActive() || m_scanPreparing || !m_scanWaiting.isEmpty()) return;
    if (m_scanCancel->load()) { finishScan("cancelled", "Library scan stopped."); return; }
    if (m_scanIndex >= m_scanShows.size()) {
        finishScan("complete", m_scan.value("failedShows").toInt() || m_scan.value("failedSeasons").toInt()
            ? "Library scan finished with errors. Review Logs for details." : "Library scan complete."); return;
    }
    const auto show = m_scanShows[m_scanIndex]; const auto selection = *m_scanSelection;
    const auto cancel = m_scanCancel; const auto generation = m_scanGeneration;
    m_scanPreparing = true; m_scanSeasons = m_scanSeasonsDone = 0;
    m_scan["currentShow"] = show.title; m_scan["currentSeason"] = 0; m_scan["message"] = "Loading show metadata…"; sync();
    m_discoveryPool.start(QRunnable::create([this, show, selection, cancel, generation] {
        std::optional<providers::ShowDetail> detail;
        try {
            detail = m_library.m_catalog.detail(m_library.m_catalog.config(selection.saved, cancel), selection.listing, show.identity.itemId, cancel);
            segments::loadCatalogSegments(m_store.databasePath(), *detail, selection.address());
        }
        catch (const providers::Cancelled &) {}
        catch (...) {}
        QMetaObject::invokeMethod(this, [this, detail, selection, generation] {
            if (m_closed || generation != m_scanGeneration) return;
            m_scanPreparing = false;
            if (m_scanCancel->load()) { advanceScan(); return; }
            if (detail) {
                m_scanDetail = detail;
                prepareShowDetection(selection, detail->show.identity);
                for (const auto &season : detail->seasons) {
                    if (!season.number || season.episodes.size() < 2) continue;
                    const auto before = QSet<QString>(m_jobs.keyBegin(), m_jobs.keyEnd());
                    const auto id = enqueue(*detail, int(*season.number), m_scanKinds, selection);
                    if (id.isEmpty()) {
                        m_scan["failedSeasons"] = m_scan.value("failedSeasons").toInt() + 1;
                        emit diagnosticMessage("warning", QString("Library scan could not queue %1, season %2.").arg(detail->show.title).arg(*season.number));
                        continue;
                    }
                    m_scanJobs.insert(id); m_scanWaiting.insert(id); ++m_scanSeasons;
                    if (!before.contains(id)) m_scanOwnedJobs.insert(id);
                    else {
                        QStringList missing;
                        for (const auto &kind : m_scanKinds) if (!m_jobs[id].kinds.contains(kind)) missing.append(kind);
                        if (!missing.isEmpty()) m_scanFollowups[id] = {int(*season.number), missing};
                    }
                }
            } else {
                m_scan["failedShows"] = m_scan.value("failedShows").toInt() + 1;
                emit diagnosticMessage("warning", "Library scan could not load metadata for " + m_scan.value("currentShow").toString() + ". Continuing with the next show.");
            }
            emit m_library.changed();
            if (m_scanWaiting.isEmpty()) {
                ++m_scanIndex; m_scan["completedShows"] = m_scanIndex; m_scan["remainingShows"] = m_scanShows.size() - m_scanIndex;
                // Yield between empty/unavailable shows, too.
                QMetaObject::invokeMethod(this, [this] { advanceScan(); }, Qt::QueuedConnection);
            }
            sync();
        }, Qt::QueuedConnection);
    }));
}
void DetectionModel::scanJobFinished(const QString &id, const QString &status)
{
    if (!libraryScanActive() || !m_scanWaiting.remove(id)) return;
    const auto followup = m_scanFollowups.take(id);
    if (!followup.second.isEmpty() && !m_scanCancel->load() && m_scanDetail) {
        const auto next = enqueue(*m_scanDetail, followup.first, followup.second, *m_scanSelection);
        if (!next.isEmpty()) { m_scanJobs.insert(next); m_scanOwnedJobs.insert(next); m_scanWaiting.insert(next); return; }
        m_scan["failedSeasons"] = m_scan.value("failedSeasons").toInt() + 1;
    }
    ++m_scanSeasonsDone;
    if (status != "complete" && !m_scanCancel->load()) m_scan["failedSeasons"] = m_scan.value("failedSeasons").toInt() + 1;
    if (m_scanWaiting.isEmpty()) {
        if (!m_scanCancel->load()) { ++m_scanIndex; m_scan["completedShows"] = m_scanIndex; m_scan["remainingShows"] = m_scanShows.size() - m_scanIndex; }
        advanceScan();
    }
}
void DetectionModel::stopLibraryScan()
{
    if (!libraryScanActive()) return;
    m_scanCancel->store(true); m_scan["status"] = "stopping"; m_scan["message"] = "Stopping library scan…";
    // A scan may join a manually queued season. Stopping the scan must not cancel that request.
    for (const auto &id : m_scanOwnedJobs) if (m_jobs.contains(id)) m_jobs[id].cancel->store(true);
    m_scanWaiting.intersect(m_scanOwnedJobs); advanceScan(); sync();
}
void DetectionModel::finishScan(const QString &status, const QString &message)
{
    m_scanClock.stop();
    m_scan["active"] = false; m_scan["status"] = status; m_scan["message"] = message;
    if (status == "complete") m_scan["progress"] = 1.;
    emit diagnosticMessage(status == "failed" ? "error" : "info", message); sync();
}
void DetectionModel::stop()
{
    const auto job = m_jobs.constFind(m_status.value("id").toString()); if (job == m_jobs.cend()) return;
    job->cancel->store(true); m_message = "Stopping detection"; emit changed();
}
void DetectionModel::finished(const QString &id, const detection::DetectionUpdates &updates, const QString &error, const QVariantMap &completed)
{
    const auto found = m_jobs.find(id); if (found == m_jobs.end()) return; if (m_closed) return;
    if (m_displayJob == id) { m_diagnosticsJob = id; m_diagnostics = completed.value("diagnostics").toString(); }
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
    scanJobFinished(id, completed.value("status").toString());
    emit m_library.changed();
    m_message = error; sync();
    if (!error.isEmpty()) { emit errorOccurred(error); return; }
    // Completion evidence was reduced on the worker after persistence/webhook
    // delivery. Publish it only after the season snapshot and edit unlock above.
    const auto &row = completed;
    const auto result = row.value("result").toMap();
    const auto failures = row.value("failures").toList();
    QString message = row.value("message").toString();
    if (row.value("status") == "complete") {
        const auto count = result.value("updated").toMap().size();
        message = count == 0 ? "No new timestamps were saved." : QString("Timestamps saved for %1 %2.").arg(count).arg(count == 1 ? "episode" : "episodes");
        if (!failures.isEmpty()) message += " Some media could not be analyzed; review the season's results.";
        else if (row.value("matchingWarnings").toBool()) message += " Some comparisons had limited evidence.";
    }
    const auto status=row.value("status").toString();
    const QMap<QString,QString> providers{{"plex","Plex"},{"emby","Emby"},{"jellyfin","Jellyfin"},{"local","Local files"}};
    const auto provider=providers.value(row.value("provider").toString(),"Media server");
    bool durationOk=false;double durationMs=result.value("durationMs").toDouble(&durationOk);
    if(!durationOk || !std::isfinite(durationMs) || durationMs<0)
        durationMs=std::max(0.,(row.value("updated").toDouble()-row.value("created").toDouble())*1000.);
    const auto outcome=status=="complete"?"Detection complete":status=="cancelled"?"Detection stopped":"Detection failed";
    emit diagnosticMessage(status=="failed"?"error":"info",QString("%1 · %2, season %3 · %4 · Duration: %5 s. %6")
        .arg(provider,row.value("title").toString()).arg(row.value("season").toInt())
        .arg(outcome,QString::number(durationMs/1000.,'f',1),message));
    emit jobFinished(row.value("title").toString(), row.value("season").toInt(), status, message);
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
}
void DetectionModel::close()
{
    if (m_closed) return; m_closed = true; m_dispatchClosing.store(true); m_scanClock.stop(); for (const auto &job : m_jobs) job.cancel->store(true);
    if (m_scanCancel) m_scanCancel->store(true);
    disconnect(&m_library, nullptr, this, nullptr); m_discoveryPool.waitForDone(); m_pool.waitForDone(); QCoreApplication::sendPostedEvents(this, QEvent::MetaCall); m_jobs.clear();m_pending.clear();
    emit m_library.changed();
}
} // namespace introvert::ui
