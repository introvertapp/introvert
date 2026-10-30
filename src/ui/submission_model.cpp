#include "submission_model.h"
#include <QCoreApplication>
#include <QEvent>
#include <QRunnable>
#include <chrono>

namespace introvert::ui {
namespace {
QString label(const QString &endpoint)
{
    return endpoint == "introdb_app" ? QString("introdb.app") : QString("theintrodb.org");
}
}

SubmissionModel::SubmissionModel(LibraryModel &library, SettingsStore settings, submissions::SubmissionStore store,
                                 segments::SegmentStore segments, QObject *parent)
    : QObject(parent), m_library(library), m_settings(std::move(settings)), m_store(std::move(store)), m_segments(std::move(segments))
{
    m_pool.setMaxThreadCount(1);
    connect(&library, &LibraryModel::changed, this, &SubmissionModel::libraryChanged);
    libraryChanged();
}

SubmissionModel::~SubmissionModel() { close(); }

bool SubmissionModel::Context::operator==(const Context &other) const
{
    return revision == other.revision && key == other.key && season == other.season && identity == other.identity;
}

std::optional<SubmissionModel::Context> SubmissionModel::context(bool includeIdentity) const
{
    if (!m_library.m_detail) return {};
    if (!includeIdentity) return Context{m_library.m_detailRevision, m_library.selection(), m_library.season(), {}};
    const auto &detail=*m_library.m_detail;
    QStringList identity{detail.show.externalIds.imdb.value_or(QString()),detail.show.externalIds.tmdb.value_or(QString()),detail.show.externalIds.tvdb.value_or(QString())};
    for(const auto &season:detail.seasons)for(const auto &episode:season.episodes)
        identity.append(episode.identity.itemId+":"+episode.numberingSource.value_or(QString())+":"+episode.episodeOrder.value_or(QString())+":"+episode.tvdbEpisodeId.value_or(QString()));
    return Context{m_library.m_detailRevision, m_library.selection(), m_library.season(),identity.join('|')};
}

void SubmissionModel::libraryChanged()
{
    if (m_closed) return;
    const auto current = context(false);
    if (current != m_feedbackContext) {
        m_feedbackContext = current; ++m_feedbackGeneration;
        m_feedbackInitialized = false; m_statusBaseline.clear(); m_message.clear();
        for (auto &value : m_statuses) {
            auto row = value.toMap(); row["segments"] = QVariantMap(); value = row;
        }
        // Reattach to known active work immediately, even while the worker is
        // awaiting an HTTP response and cannot publish another snapshot yet.
        if (m_haveStatusSnapshot) m_statuses = visibleStatuses(m_rawStatuses, m_feedbackGeneration.load());
        emit changed();
    }
    if (!m_context || context() == m_context) return;
    ++m_generation; m_plan.reset(); m_review.clear(); m_context.reset(); m_busy = false; emit changed();
}

QVariantMap SubmissionModel::visibleStatuses(QVariantMap statuses, quint64 generation)
{
    const bool stale = generation != m_feedbackGeneration.load();
    const bool current = !stale && m_feedbackContext.has_value();
    for (auto episode = statuses.begin(); episode != statuses.end(); ++episode) {
        auto row = episode.value().toMap(); QVariantMap visible;
        const auto segments = row.value("segments").toMap();
        for (auto segment = segments.begin(); segment != segments.end(); ++segment) {
            auto state = segment.value().toMap();
            const auto activity = state.take("activity").toString();
            const auto key = episode.key() + '\n' + segment.key();
            const bool active = QStringList{"WAITING", "PROCESSING", "RATE LIMITED"}.contains(state.value("result").toString());
            // A delayed terminal snapshot from before navigation must never
            // turn into fresh feedback on the next snapshot of this visit.
            if (stale && !active) m_statusBaseline[key] = activity;
            if (current && !m_feedbackInitialized && !active) m_statusBaseline[key] = activity;
            if (current && (active || m_statusBaseline.value(key) != activity)) visible[segment.key()] = state;
        }
        row["segments"] = visible; episode.value() = row;
    }
    if (current) m_feedbackInitialized = true;
    return stale ? m_statuses : statuses;
}

void SubmissionModel::start()
{
    if (m_closed || m_started) return;
    m_started = true;
    m_pool.start(QRunnable::create([this] { run(); }));
}

void SubmissionModel::push(Command command)
{
    { std::lock_guard<std::mutex> lock(m_mutex); m_commands.push_back(std::move(command)); }
    start(); m_wake.notify_one();
}

void SubmissionModel::prepare(const QVariantList &selection, const QString &endpoint)
{
    if (m_closed || m_busy) return;
    ++m_generation; m_plan.reset(); m_review.clear(); m_message.clear();
    if (!m_library.m_detail) {
        m_message = "Open a TV show before submitting segments"; emit changed(); return;
    }
    m_context = context(); m_busy = true;
    auto key = endpoint;
    for (const auto &id : {QString("introdb_app"), QString("theintrodb_org")}) if (endpoint == label(id)) key = id;
    push({Kind::Prepare, m_generation, m_library.m_detail, selection, key, {}});
    emit changed();
}

void SubmissionModel::confirm()
{
    if (m_closed || !ready()) return;
    if (context() != m_context) { cancelReview(); return; }
    m_busy = true;
    push({Kind::Enqueue, m_generation, {}, {}, {}, *m_plan});
    m_plan.reset(); emit changed();
}

void SubmissionModel::cancelReview()
{
    if (m_closed) return;
    ++m_generation; m_plan.reset(); m_review.clear(); m_context.reset();
    m_busy = false; m_message.clear(); emit changed();
}

void SubmissionModel::run()
{
    const auto post = [this](Kind kind, quint64 generation, const QVariant &value, const QString &error = {}) {
        QMetaObject::invokeMethod(this, [this, kind, generation, value, error] {
            finished(kind, generation, value, error);
        }, Qt::QueuedConnection);
    };
    QVariantMap previous; std::optional<quint64> previousGeneration;
    const auto publish = [&] {
        const auto generation = m_feedbackGeneration.load();
        const auto settings = m_settings.loadEndpoints();
        QVariantList endpoints;
        for (const auto &id : {QString("introdb_app"), QString("theintrodb_org")}) {
            const auto enabled = settings.value(id == "introdb_app" ? "introdb_enabled" : "theintrodb_enabled").toBool();
            if (enabled && m_settings.hasCredential("endpoints:" + id)) endpoints.append(label(id));
        }
        const auto snapshot = m_store.snapshot();
        const QVariantMap value{{"queue", snapshot.queue}, {"history", snapshot.history}, {"statistics", snapshot.statistics},
            {"endpoints", endpoints}, {"statuses", m_store.episodeStatuses(true)}};
        if (value != previous || previousGeneration != generation) {
            previous = value; previousGeneration = generation; post(Kind::Snapshot, generation, value);
        }
    };
    while (!m_stop->load()) {
        try {
            std::optional<Command> command;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (!m_commands.empty()) { command = std::move(m_commands.front()); m_commands.pop_front(); }
            }
            if (command) {
                try {
                    QVariant value;
                    if (command->kind == Kind::Prepare) {
                        value = submissions::prepareSubmission(*command->detail, command->selection, command->endpoint, m_settings, m_segments, m_stop);
                    } else {
                        const auto client = submissions::EndpointClient::fromStore(command->plan.value("endpoint").toString(), m_settings);
                        for (const auto &item : command->plan.value("items").toList()) client.validate(item.toMap());
                        value = m_store.enqueue(command->plan);
                    }
                    post(command->kind, command->generation, value);
                } catch (const submissions::SubmissionError &error) {
                    post(command->kind, command->generation, {}, QString::fromUtf8(error.what()));
                } catch (...) {
                    post(command->kind, command->generation, {}, "Cannot access submission settings or app.db. Check database access and retry.");
                }
            }
            publish();
            m_store.processOne(m_settings, m_stop, publish);
        } catch (...) {
            if (!m_stop->load()) post(Kind::Error, 0, {}, "Submission processing could not access app.db or endpoint configuration. Pending jobs remain stored and will retry.");
        }
        std::unique_lock<std::mutex> lock(m_mutex);
        m_wake.wait_for(lock, std::chrono::milliseconds(200), [this] { return m_stop->load() || !m_commands.empty(); });
    }
}

void SubmissionModel::finished(Kind kind, quint64 generation, const QVariant &value, const QString &error)
{
    if (m_closed) return;
    if (kind == Kind::Snapshot) {
        const auto row = value.toMap(); m_queue = row.value("queue").toList(); m_history = row.value("history").toList();
        m_statistics = row.value("statistics").toMap(); m_endpoints = row.value("endpoints").toList();
        m_rawStatuses = row.value("statuses").toMap(); m_haveStatusSnapshot = true;
        m_statuses = visibleStatuses(m_rawStatuses, generation);
    } else if (kind == Kind::Error) m_message = error;
    else if (generation == m_generation) {
        m_busy = false; m_message = error;
        if (value.isValid()) {
            if (kind == Kind::Prepare) {
                m_plan = value.toMap(); const auto &plan = *m_plan;
                QVariantMap types; QSet<QString> episodes;
                const auto items = plan.value("items").toList();
                for (const auto &value : items) {
                    const auto item = value.toMap(); auto type = item.value("segment_type").toString(); type[0] = type[0].toUpper();
                    types[type] = types.value(type).toInt() + 1; episodes.insert(item.value("episode_id").toString());
                }
                const auto identifier = [&](const QString &name) -> QVariant {
                    const auto id = plan.value(name); return !id.isValid() || id.isNull() || id.toString().isEmpty() ? QVariant(QString::fromUtf8("—")) : id;
                };
                m_review = {{"show", plan.value("show")}, {"season", plan.value("season")}, {"imdb", identifier("imdb")}, {"tmdb", identifier("tmdb")}, {"tvdb", identifier("tvdb")}, {"targets", items},
                    {"endpoint", label(plan.value("endpoint").toString())}, {"episodes", episodes.size()}, {"segments", items.size()}, {"types", types}};
                emit prepared();
            } else { m_review.clear(); m_context.reset(); emit enqueued(); }
        }
    }
    emit changed();
}

void SubmissionModel::close()
{
    if (m_closed) return;
    m_closed = true; m_stop->store(true); m_wake.notify_one();
    disconnect(&m_library, nullptr, this, nullptr);
    m_pool.waitForDone();
    QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
}

} // namespace introvert::ui
