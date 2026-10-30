#include "service.h"
#include "candidates.h"
#include "segments/identity.h"
#include "core/sqlite.h"
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>

namespace introvert::detection {
namespace {
double now() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }
QString json(const QVariantMap &value) { return QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(value)).toJson(QJsonDocument::Compact)); }
QVariantList parameters(const QStringList &keys, const providers::Episode &episode, const QString &kind) {
    QVariantList result; for (const auto &key : keys) result.append(key); result.append(episode.seasonNumber ? QVariant(*episode.seasonNumber) : QVariant{}); result.append(episode.episodeNumber ? QVariant(*episode.episodeNumber) : QVariant{}); result.append(kind); return result;
}
}
DetectionService::DetectionService(DetectionJobStore store, segments::SegmentStore segments, std::shared_ptr<MediaDetectionRunner> runner, Config config, Completion completion)
    : m_store(std::move(store)), m_segments(std::move(segments)), m_runner(std::move(runner)), m_config(std::move(config)), m_completion(std::move(completion)) {}
void DetectionService::completed(const QString &id, const std::function<void()> &notify) const
{
    if (notify) notify(); if (!m_completion) return;
    try {
        for (const auto &value : m_store.rows()) {
            const auto job = value.toMap(); if (job.value("id") != id) continue;
            auto result = QJsonDocument::fromJson(job.value("result").toString().toUtf8()).object().toVariantMap(); result["webhook"] = m_completion(job);
            sql::Connection connection(m_store.databasePath()); sql::execute(connection.db, "UPDATE detection_jobs SET result=? WHERE id=?", {json(result), id}); break;
        }
    } catch (...) {} // Delivery/storage failure cannot undo the committed scan.
}
DetectionUpdates DetectionService::run(const QString &id, const providers::ShowDetail &detail, int season, const QStringList &kinds,
    const ProviderSettings &saved, const providers::Cancellation &cancel, const std::function<void()> &notify) const
{
    QElapsedTimer timer; timer.start();
    try {
        // A queued scan can wait for an already-started deletion to commit.
        // Snapshot status visibility after those writers have drained, too.
        {
            sql::Connection connection(m_store.databasePath());
            sql::execute(connection.db,"UPDATE detection_jobs SET status_revision=? WHERE id=? AND status='queued'",
                {segments::currentRevision(connection.db),id});
        }
        QList<providers::Episode> episodes; for (const auto &group : detail.seasons) if (group.number == season) { episodes = group.episodes; break; }
        if (episodes.size() < 2) throw std::invalid_argument("Need at least 2 episodes in the season to detect segments");
        const auto keys = segments::showKeys(detail.show); QStringList marks; for (const auto &key : keys) { Q_UNUSED(key); marks.append("?"); }
        const auto episodeWhere = "show_key IN (" + marks.join(',') + ") AND season=? AND episode=?";
        const auto where = episodeWhere + " AND kind=?";
        std::map<std::pair<QString, QString>, qint64> baseline;
        QMap<QString, segments::SegmentValues> knownSegments;
        for (const auto &episode : episodes) {
            knownSegments[episode.identity.itemId] = m_segments.load(detail.show, episode);
            sql::Connection connection(m_store.databasePath()); for (const auto &kind : kinds) {
                auto query = sql::execute(connection.db, "SELECT COALESCE(MAX(revision),0) FROM segments WHERE " + where, parameters(keys, episode, kind));
                query.next(); baseline[{episode.identity.itemId, kind}] = query.value(0).toLongLong();
            }
        }
        providers::checkCancelled(cancel);
        const auto progress = [&](const QString &phase, int current, int total) { m_store.progress(id, phase, current, total); if (notify) notify(); };
        progress("loading media", 0, int(episodes.size())); const auto config = m_config(saved, cancel); const auto output = m_runner->run(config, episodes, kinds, cancel, progress, id);
        providers::checkCancelled(cancel);
        if (output.failed) {
            const auto message = output.diagnostics.value("usable_episodes").toInt() >= 2 ? "Audio matching failed. See job diagnostics."
                : "Audio analysis could not compare usable episode audio. See job diagnostics.";
            m_store.finish(id, "failed", message, {}, output.diagnostics); return {};
        }
        providers::checkCancelled(cancel); progress("saving results", 0, 1); QVariantMap applied, eligible; QVariantList skipped, finalization;
        auto diagnostics = output.diagnostics;
        {
            sql::Connection connection(m_store.databasePath()); sql::Transaction transaction(connection.db);
            for (const auto &episode : episodes) {
                segments::SegmentChanges changes;
                auto currentSegments = knownSegments.value(episode.identity.itemId);
                if (output.finalize) {
                    auto arguments = parameters(keys, episode, {}); arguments.removeLast();
                    auto current = sql::execute(connection.db, "SELECT kind,start,end,deleted FROM segments WHERE " + episodeWhere + " ORDER BY revision DESC", arguments);
                    QSet<QString> seen;
                    while (current.next()) {
                        const auto kind = current.value(0).toString(); if (seen.contains(kind)) continue; seen.insert(kind);
                        if (current.value(3).toBool()) currentSegments.remove(kind);
                        else currentSegments[kind] = {current.value(1).toDouble(), current.value(2).toDouble()};
                    }
                }
                for (const auto &kind : kinds) {
                    const auto episodeValues = output.episodes.constFind(episode.identity.itemId); if (episodeValues == output.episodes.cend()) continue;
                    const auto value = episodeValues->constFind(kind); if (value == episodeValues->cend() || !std::isfinite(value->first) || !std::isfinite(value->second) || value->first < 0 || value->first >= value->second
                        || (episode.durationMs && *episode.durationMs && value->second > *episode.durationMs / 1000.)) continue;
                    const auto arguments = parameters(keys, episode, kind); auto revision = sql::execute(connection.db, "SELECT COALESCE(MAX(revision),0) FROM segments WHERE " + where, arguments); revision.next(); const auto version = revision.value(0).toLongLong(); revision.finish();
                    // Reruns replace pre-existing values/deletions of any source,
                    // while retaining changes made after this scan's snapshot.
                    if (version != baseline[{episode.identity.itemId, kind}]) { skipped.append(QVariantMap{{"id", episode.identity.itemId}, {"kind", kind}, {"reason", "changed_during_detection"}}); continue; }
                    auto interval = *value;
                    if (output.finalize) {
                        const auto finalized = output.finalize(episode.identity.itemId, kind, interval, currentSegments, cancel);
                        interval = finalized.interval;
                        if (!std::isfinite(interval.first) || !std::isfinite(interval.second) || interval.first < 0 || interval.first >= interval.second)
                            throw std::invalid_argument("Invalid finalized segment");
                        auto evidence = finalized.diagnostics; evidence["episode"] = episode.identity.itemId; evidence["segment_type"] = kind;
                        finalization.append(evidence);
                    }
                    changes[kind] = interval;
                }
                if (!changes.empty()) {
                    segments::SegmentStore::writeInTransaction(connection.db, detail.show, episode, changes, "auto_detect"); QVariantMap values;
                    for (auto it = changes.cbegin(); it != changes.cend(); ++it) values[it.key()] = QVariantList{it.value()->first, it.value()->second}; applied[episode.identity.itemId] = values;
                }
            }
            providers::checkCancelled(cancel); for (const auto &episode : episodes) eligible[episode.identity.itemId] = kinds;
            const QVariantMap result{{"updated", applied}, {"skipped", skipped}, {"eligible", eligible}, {"segmentTypes", kinds}, {"durationMs", quantizedTimestamp(timer.nsecsElapsed() / 1e6, 1)}};
            const auto message = !output.diagnostics.value("failures").toList().empty() ? "Detection complete with failures"
                : !output.diagnostics.value("matching_warnings").toList().empty() ? "Detection complete with limited matching evidence" : "Detection complete";
            if (output.finalize) diagnostics["finalization"] = finalization;
            sql::execute(connection.db, "UPDATE detection_jobs SET status='complete',current=total,message=?,result=?,diagnostics=?,updated=? WHERE id=?", {message, json(result), json(diagnostics), now(), id});
            providers::checkCancelled(cancel); transaction.commit();
        }
        completed(id, notify); DetectionUpdates updates;
        for (const auto &episode : episodes) updates[episode.identity.itemId] = {m_segments.load(detail.show, episode), m_segments.isEdited(detail.show, episode)}; return updates;
    } catch (const providers::Cancelled &) { m_store.finish(id, "cancelled", "Detection stopped"); return {}; }
    catch (const providers::ProviderError &error) { m_store.finish(id, "failed", QString::fromUtf8(error.what())); return {}; }
    catch (const std::invalid_argument &error) { m_store.finish(id, "failed", QString::fromUtf8(error.what())); return {}; }
    catch (...) { m_store.finish(id, "failed", "Detection failed. Check media access and the required detector runtime."); return {}; }
}
} // namespace introvert::detection
