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
}

SubmissionModel::~SubmissionModel() { close(); }

bool SubmissionModel::Context::operator==(const Context &other) const
{
    return revision == other.revision && key == other.key && season == other.season && identity == other.identity;
}

std::optional<SubmissionModel::Context> SubmissionModel::context() const
{
    if (!m_library.m_detail) return {};
    const auto &detail=*m_library.m_detail;
    QStringList identity{detail.show.externalIds.imdb.value_or(QString()),detail.show.externalIds.tmdb.value_or(QString()),detail.show.externalIds.tvdb.value_or(QString())};
    for(const auto &season:detail.seasons)for(const auto &episode:season.episodes)
        identity.append(episode.identity.itemId+":"+episode.numberingSource.value_or(QString())+":"+episode.episodeOrder.value_or(QString())+":"+episode.tvdbEpisodeId.value_or(QString()));
    return Context{m_library.m_detailRevision, m_library.selection(), m_library.season(),identity.join('|')};
}

void SubmissionModel::libraryChanged()
{
    if (m_closed || !m_context || context() == m_context) return;
    ++m_generation; m_plan.reset(); m_review.clear(); m_context.reset(); m_busy = false; emit changed();
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
    QVariantMap previous;
    const auto publish = [&] {
        const auto settings = m_settings.loadEndpoints();
        QVariantList endpoints;
        for (const auto &id : {QString("introdb_app"), QString("theintrodb_org")}) {
            const auto enabled = settings.value(id == "introdb_app" ? "introdb_enabled" : "theintrodb_enabled").toBool();
            if (enabled && m_settings.hasCredential("endpoints:" + id)) endpoints.append(label(id));
        }
        const auto snapshot = m_store.snapshot();
        const QVariantMap value{{"queue", snapshot.queue}, {"history", snapshot.history}, {"statistics", snapshot.statistics},
            {"endpoints", endpoints}, {"statuses", m_store.episodeStatuses()}};
        if (value != previous) { previous = value; post(Kind::Snapshot, 0, value); }
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
        m_statistics = row.value("statistics").toMap(); m_endpoints = row.value("endpoints").toList(); m_statuses = row.value("statuses").toMap();
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
