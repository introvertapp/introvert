#include "submission_model.h"
#include <QCoreApplication>
#include <QEvent>
#include "segments/identity.h"
#include <QRunnable>
#include <chrono>
#include <cmath>

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
    m_queuePool.setMaxThreadCount(1);
    m_endpointPool.setMaxThreadCount(2);
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
    const auto *active = m_library.activeDetail();
    if (!active) return {};
    const auto seasonNumber = active->show.isMovie() ? int(segments::movieCoordinate) : m_library.season();
    if (!includeIdentity) return Context{m_library.m_detailRevision, m_library.selection(), seasonNumber, {}};
    const auto &detail=*active;
    QStringList identity{detail.show.externalIds.imdb.value_or(QString()),detail.show.externalIds.tmdb.value_or(QString()),detail.show.externalIds.tvdb.value_or(QString())};
    for(const auto &season:detail.seasons)for(const auto &episode:season.episodes)
        identity.append(episode.identity.itemId+":"+episode.numberingSource.value_or(QString())+":"+episode.episodeOrder.value_or(QString())+":"+episode.tvdbEpisodeId.value_or(QString()));
    return Context{m_library.m_detailRevision, m_library.selection(), seasonNumber,identity.join('|')};
}

void SubmissionModel::libraryChanged()
{
    if (m_closed) return;
    const auto current = context(false);
    const auto *detail = m_library.activeDetail();
    const auto canonical = current ? segments::showKeys(detail->show) : QStringList();
    if (current != m_feedbackContext || canonical != m_feedbackCanonical) {
        const bool sameShow = current && m_feedbackContext && current->revision == m_feedbackContext->revision
            && current->key == m_feedbackContext->key && canonical == m_feedbackCanonical;
        if (!sameShow) m_progressStatuses.clear();
        m_feedbackContext = current;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_feedbackScope = current ? providers::providerScope(detail->show.identity,
                current->key ? (current->key->saved.provider == "local" ? current->key->saved.localRoot : current->key->saved.remoteUrl) : QString()) : QString();
            m_feedbackCanonical = canonical;
            ++m_feedbackGeneration;
        }
        m_feedbackInitialized = false; m_statusBaseline.clear(); m_message.clear();
        for (auto &value : m_statuses) {
            auto row = value.toMap(); row["segments"] = QVariantMap(); value = row;
        }
        // Reattach to known active work immediately, even while the worker is
        // awaiting an HTTP response and cannot publish another snapshot yet.
        if (m_haveStatusSnapshot && m_rawStatusScope == m_feedbackScope && m_rawStatusCanonical == m_feedbackCanonical)
            m_statuses = visibleStatuses(m_rawStatuses, m_feedbackGeneration.load());
        else m_statuses.clear();
        m_wake.notify_one();
        emit changed();
    }
    if (!m_context || context() == m_context) return;
    if (m_reviewCancel) m_reviewCancel->store(true);
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
            const auto key = episode.key() + '\n' + state.value("segmentKey", state.value("kind")).toString() + ':' + state.value("endpoint").toString();
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

QVariantMap SubmissionModel::seasonProgress() const
{
    QMap<QString, QPair<int, int>> counts;
    const auto *detail = m_library.activeDetail();
    if (!m_feedbackContext || !detail) return {};
    const auto prefix = (detail->show.isMovie() ? QStringLiteral("movie:") : QString())
        + providers::providerName(detail->show.identity.provider) + ':' + detail->show.identity.itemId + ':';
    for (auto episode = m_progressStatuses.begin(); episode != m_progressStatuses.end(); ++episode) {
        if (!episode.key().startsWith(prefix)) continue;
        const auto row = episode.value().toMap();
        const auto states = row.value("segments").toMap();
        if (states.isEmpty()) continue;
        auto &count = counts[row.value("season").toString()];
        for (const auto &entry : states) {
            ++count.second;
            if (!QStringList{"WAITING", "PROCESSING", "RATE LIMITED"}.contains(entry.toMap().value("result").toString())) ++count.first;
        }
    }
    QVariantMap result;
    for (auto it = counts.begin(); it != counts.end(); ++it)
        result[it.key()] = QVariantMap{{"done", it->first}, {"total", it->second}, {"progress", it->second ? double(it->first) / it->second : -1.}};
    return result;
}

double SubmissionModel::progress() const
{
    int done = 0, total = 0;
    for (const auto &entry : seasonProgress()) { const auto row = entry.toMap(); done += row.value("done").toInt(); total += row.value("total").toInt(); }
    return total ? double(done) / total : -1.;
}

void SubmissionModel::start()
{
    if (m_closed || m_started) return;
    m_started = true;
    m_pool.start(QRunnable::create([this] { run(); }));
    for (const auto &endpoint : {QString("introdb_app"), QString("theintrodb_org")})
        m_endpointPool.start(QRunnable::create([this, endpoint] {
            while (!m_stop->load()) {
                try { m_store.processOne(m_settings, m_stop, {}, endpoint); }
                catch (...) {
                    if (!m_stop->load()) QMetaObject::invokeMethod(this, [this] {
                        finished(Kind::Error, 0, {}, "Submission processing could not access app.db or endpoint configuration. Pending jobs remain stored and will retry.");
                    }, Qt::QueuedConnection);
                }
                std::unique_lock<std::mutex> lock(m_mutex);
                m_wake.wait_for(lock, std::chrono::milliseconds(200));
            }
        }));
}

void SubmissionModel::push(Command command)
{
    { std::lock_guard<std::mutex> lock(m_mutex); m_commands.push_back(std::move(command)); }
    start(); m_wake.notify_one();
}

void SubmissionModel::prepare(const QVariantList &selection, const QString &endpoint)
{
    prepareMany(selection, {endpoint});
}

void SubmissionModel::prepareMany(const QVariantList &selection, const QVariantList &endpoints)
{
    if (m_closed || m_busy) return;
    if (m_reviewCancel) m_reviewCancel->store(true);
    m_reviewCancel = std::make_shared<std::atomic_bool>(false);
    ++m_generation; m_plan.reset(); m_review.clear(); m_message.clear();
    const auto *detail = m_library.activeDetail();
    if (!detail) {
        m_message = m_library.m_browseMovies ? "Open a movie before submitting segments" : "Open a TV show before submitting segments";
        emit changed(); return;
    }
    m_context = context(); m_busy = true;
    QVariantList keys;
    for (const auto &value : endpoints) {
        auto key = value.toString();
        for (const auto &id : {QString("introdb_app"), QString("theintrodb_org")}) if (key == label(id)) key = id;
        if (!keys.contains(key)) keys.append(key);
    }
    push({Kind::Prepare, m_generation, *detail, selection, keys, {}, m_reviewCancel, m_feedbackScope});
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
    if (m_reviewCancel) m_reviewCancel->store(true);
    ++m_generation; m_plan.reset(); m_review.clear(); m_context.reset();
    m_busy = false; m_message.clear(); emit changed();
}

void SubmissionModel::moveQueueItem(const QString &key, const QString &target, bool after, const QStringList &order)
{
    if (m_closed || key.isEmpty() || target.isEmpty()) return;
    // Reordering must remain responsive during an endpoint's HTTP request.
    m_queuePool.start(QRunnable::create([this, key, target, after, order] {
        QString error; QVariantMap positions;
        try {
            m_store.moveQueueItem(key, target, after, order);
            sql::Connection connection(m_settings.databasePath());
            auto query = sql::execute(connection.db, "SELECT key,position FROM queue_order");
            while (query.next()) positions[query.value(0).toString()] = query.value(1);
        }
        catch (...) { error = "Cannot save the queue order in app.db. Please retry."; }
        QMetaObject::invokeMethod(this, [this, error, positions] {
            if (m_closed) return;
            if (!error.isEmpty()) emit errorOccurred(error);
            m_queuePositions = positions;
            emit queueOrderChanged();
            m_wake.notify_one();
        }, Qt::QueuedConnection);
    }));
}

void SubmissionModel::run()
{
    const auto post = [this](Kind kind, quint64 generation, const QVariant &value, const QString &error = {}) {
        QMetaObject::invokeMethod(this, [this, kind, generation, value, error] {
            finished(kind, generation, value, error);
        }, Qt::QueuedConnection);
    };
    QVariantMap previous, cached; std::optional<quint64> previousGeneration, cachedGeneration;
    std::optional<qint64> cachedRevision;
    std::unique_ptr<sql::Connection> watch;
    QMap<QString, double> limits;
    QMap<QString, QString> limitOrigins;

    const auto publish = [&] {
        if (!watch) watch = std::make_unique<sql::Connection>(m_settings.databasePath());
        auto token = sql::execute(watch->db, "SELECT revision FROM submission_state WHERE id=1");
        if (!token.next()) throw DatabaseError("Submission state is missing.");
        const auto revision = token.value(0).toLongLong(); token.finish();
        quint64 generation; QString scope; QStringList canonical;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            generation = m_feedbackGeneration.load(); scope = m_feedbackScope; canonical = m_feedbackCanonical;
        }
        const bool changed = !cachedRevision || *cachedRevision != revision;
        if (changed) {
            const auto settings = m_settings.loadEndpoints(); QVariantList endpoints;
            for (const auto &id : {QString("introdb_app"), QString("theintrodb_org")}) {
                const auto enabled = settings.value(id == "introdb_app" ? "introdb_enabled" : "theintrodb_enabled").toBool();
                if (enabled && m_settings.hasCredential("endpoints:" + id)) endpoints.append(label(id));
            }
            const auto snapshot = m_store.snapshot();
            cached = {{"queue", snapshot.queue}, {"history", snapshot.history}, {"statistics", snapshot.statistics}, {"endpoints", endpoints}};
            limits.clear(); limitOrigins.clear();
            auto query = sql::execute(watch->db, "SELECT endpoint,resume_at FROM submit_api_rate_limits");
            while (query.next()) limits[label(query.value(0).toString())] = query.value(1).toDouble();
            auto origins = sql::execute(watch->db,
                "SELECT j.endpoint,j.id,i.episode_id FROM submit_items i JOIN submit_jobs j ON j.id=i.job_id "
                "JOIN submit_api_rate_limits l ON l.endpoint=j.endpoint WHERE i.status='RATE LIMITED' "
                "AND j.status IN ('queued','running','rate_limited') ORDER BY i.updated DESC,i.id DESC");
            while (origins.next()) {
                const auto endpoint = label(origins.value(0).toString());
                if (!limitOrigins.contains(endpoint)) limitOrigins[endpoint] = "s:" + origins.value(1).toString() + ':' + origins.value(2).toString();
            }
        }
        if (changed || cachedGeneration != generation) {
            cached["statuses"] = m_store.episodeStatuses(true, scope, canonical); cached["scope"] = scope;
            cached["canonical"] = canonical;
            cachedGeneration = generation;
        }
        cachedRevision = revision;
        auto value = cached;
        auto statuses = value.value("statuses").toMap();
        for (auto &episode : statuses) {
            auto row = episode.toMap(); auto segments = row.value("segments").toMap();
            for (auto &entry : segments) {
                auto state = entry.toMap();
                const auto resume = limits.value(state.value("endpoint").toString(), 0.);
                const auto seconds = qint64(std::max(0., std::ceil(resume - m_store.currentTime())));
                state["limitActive"] = seconds > 0;
                if (seconds > 0 && state.value("rateLimitOrigin").toBool()) state["cooldownSeconds"] = seconds;
                entry = state;
            }
            row["segments"] = segments; episode = row;
        }
        value["statuses"] = statuses;
        if (!limits.isEmpty()) {
            auto queue = value.value("queue").toList();
            for (auto &entry : queue) {
                auto row = entry.toMap();
                if (!row.value("status").toString().startsWith("Rate limited")) continue;
                const auto endpoint = row.value("endpoint").toString();
                const auto seconds = qint64(std::max(0., limits.value(endpoint) - m_store.currentTime()));
                row["status"] = limitOrigins.value(endpoint) == row.value("queueKey").toString()
                    ? QString("Rate limited %1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QChar('0')) : QString("Rate limited");
                entry = row;
            }
            value["queue"] = queue;
        }
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
                        providers::checkCancelled(command->cancel);
                        if (command->endpoints.isEmpty()) throw submissions::SubmissionError("Choose a submission endpoint");
                        QVariantList plans;
                        for (const auto &endpoint : command->endpoints) {
                            QMap<int, QVariantList> seasons;
                            for (const auto &entry : command->selection) {
                                const auto id = entry.toMap().value("id").toString();
                                bool found = false;
                                for (const auto &season : command->detail->seasons) for (const auto &episode : season.episodes)
                                    if (episode.identity.itemId == id) { seasons[int(season.number.value_or(-1))].append(entry); found = true; }
                                if (!found) throw submissions::SubmissionError("Invalid episode selection");
                            }
                            if (seasons.isEmpty()) throw submissions::SubmissionError("Select at least one segment");
                            for (const auto &selection : seasons)
                                plans.append(submissions::prepareSubmission(*command->detail, selection, endpoint.toString(), m_settings, m_segments, command->cancel, command->scope));
                        }
                        auto combined = plans.first().toMap(); QVariantList items, targets; QStringList labels, seasons;
                        for (const auto &entry : plans) {
                            const auto plan = entry.toMap(); items.append(plan.value("items").toList());
                            if (plan.value("endpoint") == "theintrodb_org") targets.append(plan.value("items").toList());
                            if (!labels.contains(label(plan.value("endpoint").toString()))) labels.append(label(plan.value("endpoint").toString()));
                            if (!seasons.contains(plan.value("season").toString())) seasons.append(plan.value("season").toString());
                        }
                        combined["targets"] = targets; combined["plans"] = plans; combined["items"] = items; combined["endpointLabel"] = labels.join(", "); combined["season"] = seasons.join(", ");
                        value = combined;
                    } else {
                        const auto plans = command->plan.value("plans").toList();
                        for (const auto &entry : plans) {
                            const auto plan = entry.toMap();
                            const auto client = submissions::EndpointClient::fromStore(plan.value("endpoint").toString(), m_settings);
                            for (const auto &item : plan.value("items").toList()) client.validate(item.toMap());
                        }
                        value = m_store.enqueueMany(plans);
                    }
                    post(command->kind, command->generation, value);
                } catch (const providers::Cancelled &) {
                    // Obsolete preparation never cancels confirmed queue commands.
                } catch (const submissions::SubmissionError &error) {
                    post(command->kind, command->generation, {}, QString::fromUtf8(error.what()));
                } catch (...) {
                    post(command->kind, command->generation, {}, "Cannot access submission settings or app.db. Check database access and retry.");
                }
            }
            publish();

        } catch (...) {
            watch.reset(); cachedRevision.reset();
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
        const auto failureKey = [](const QVariantMap &row) {
            return row.value("jobId").toString() + '\n' + row.value("itemId", row.value("code")).toString() + '\n' + row.value("error").toString();
        };
        QSet<QString> previousFailures;
        for (const auto &entry : m_queue + m_history) {
            const auto row = entry.toMap(); if (row.value("status") == "Fail") previousFailures.insert(failureKey(row));
        }
        const auto row = value.toMap(); m_queue = row.value("queue").toList(); m_history = row.value("history").toList();
        // The first snapshot is history, not a new failure. Report subsequent
        // endpoint failures even when their show is no longer on screen.
        if (m_haveStatusSnapshot) for (const auto &entry : m_queue + m_history) {
            const auto failed = entry.toMap(); const auto key = failureKey(failed);
            if (failed.value("status") != "Fail" || previousFailures.contains(key)) continue;
            previousFailures.insert(key);
            emit errorOccurred(QString("Could not submit %1, %2 to %3. %4").arg(failed.value("show").toString(),
                failed.value("code").toString(), failed.value("endpoint") == "introdb.app" ? QString("IntroDB") : QString("TheIntroDB"), failed.value("error").toString()));
        }
        m_statistics = row.value("statistics").toMap(); m_endpoints = row.value("endpoints").toList();
        m_rawStatuses = row.value("statuses").toMap(); m_haveStatusSnapshot = true;
        m_rawStatusScope = row.value("scope").toString();
        m_rawStatusCanonical = row.value("canonical").toStringList();
        m_statuses = visibleStatuses(m_rawStatuses, generation);
        // Card feedback remains visit-scoped; progress follows work already seen
        // on this show even when navigation clears terminal card messages.
        if (generation == m_feedbackGeneration.load()) for (auto episode = m_rawStatuses.begin(); episode != m_rawStatuses.end(); ++episode) {
            auto progressRow = m_progressStatuses.value(episode.key()).toMap();
            auto tracked = progressRow.value("segments").toMap();
            const auto raw = episode.value().toMap();
            const auto visible = m_statuses.value(episode.key()).toMap().value("segments").toMap();
            const auto states = raw.value("segments").toMap();
            for (auto entry = states.begin(); entry != states.end(); ++entry) {
                const auto state = entry.value().toMap();
                const auto key = state.value("segmentKey", state.value("kind")).toString() + ':' + state.value("endpoint").toString();
                if (tracked.contains(key) || visible.contains(entry.key())) tracked[key] = state;
            }
            if (!tracked.isEmpty()) {
                progressRow["season"] = raw.value("season"); progressRow["segments"] = tracked;
                m_progressStatuses[episode.key()] = progressRow;
            }
        }
    } else if (kind == Kind::Error) {
        if (m_message != error) emit errorOccurred(error);
        m_message = error;
    }
    else if (generation == m_generation) {
        m_busy = false; m_message = error;
        if (!error.isEmpty()) emit errorOccurred(error);
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
                m_review = {{"show", plan.value("show")}, {"season", plan.value("season")}, {"imdb", identifier("imdb")}, {"tmdb", identifier("tmdb")}, {"tvdb", identifier("tvdb")}, {"targets", plan.value("targets")},
                    {"endpoint", plan.value("endpointLabel")}, {"episodes", episodes.size()}, {"segments", items.size()}, {"types", types}};
                if (plan.value("movie").toBool()) m_review["movie"] = true;
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
    if (m_reviewCancel) m_reviewCancel->store(true);
    disconnect(&m_library, nullptr, this, nullptr);
    m_queuePool.waitForDone();
    m_endpointPool.waitForDone();
    m_pool.waitForDone();
    QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
}

} // namespace introvert::ui
