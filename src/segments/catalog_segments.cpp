#include "catalog_segments.h"
#include "identity.h"
#include "core/sqlite.h"
#include <cmath>
#include <algorithm>
#include <map>
#include <tuple>

namespace introvert::segments {

CatalogSegments loadCatalogSegments(const QString &databasePath, const providers::ShowDetail &detail,
    const QString &source, const EditLocks::Lease *reservation)
{
    CatalogSegments result;
    const auto keys = showKeys(detail.show);
    auto locks=EditLocks::forDatabase(databasePath);
    QSet<qint64> checked;
    QList<std::shared_ptr<EditLocks::Lease>> imports;
    for(const auto &group:detail.seasons)for(const auto &episode:group.episodes) {
        if(keys.isEmpty() || !episode.seasonNumber || checked.contains(*episode.seasonNumber))continue;
        const auto season=*episode.seasonNumber;checked.insert(season);
        if(locks.ownsDetection(reservation,detail.show,season,source))continue;
        auto lease=locks.edit({{detail.show,season}},source);
        if(lease)imports.append(std::move(lease));else result.deferredSeasons.insert(season);
    }
    // Acquire before SQLite: a queued detector waits for existing imports to
    // finish, while new imports for its season remain read-only. Never hold a
    // database transaction while waiting for a reservation.
    struct Stored { double start, end; bool deleted; QString source; QVariant chapterStart; };
    std::map<std::tuple<qint64, qint64, QString>, Stored> latest;
    std::map<std::pair<qint64,qint64>,qint64> resets;
    sql::Connection connection(databasePath);
    sql::Transaction transaction(connection.db);
    initializeTables(connection.db);
    if (!keys.isEmpty()) {
        QStringList placeholders;
        QVariantList arguments;
        for (const auto &key : keys) { placeholders.append("?"); arguments.append(key); }
        auto query = sql::execute(connection.db, "SELECT s.season,s.episode,s.kind,s.start,s.end,s.deleted,s.source,c.start_ms FROM segments s "
            "LEFT JOIN movie_segment_chapters c ON s.season=-1 AND c.show_key=s.show_key AND c.kind=s.kind "
            "WHERE s.show_key IN (" + placeholders.join(',') + ") ORDER BY s.revision DESC", arguments);
        while (query.next())
            latest.emplace(std::make_tuple(query.value(0).toLongLong(), query.value(1).toLongLong(), query.value(2).toString()),
                Stored{query.value(3).toDouble(), query.value(4).toDouble(), query.value(5).toBool(), query.value(6).toString(), query.value(7)});
        auto cleared=sql::execute(connection.db,"SELECT season,episode,MAX(revision) FROM episode_status_resets WHERE show_key IN ("
            +placeholders.join(',')+") GROUP BY season,episode",arguments);
        while(cleared.next())resets[{cleared.value(0).toLongLong(),cleared.value(1).toLongLong()}]=cleared.value(2).toLongLong();
    }
    for (const auto &season : detail.seasons) for (const auto &episode : season.episodes) {
        SegmentValues row;
        const auto coordinates = segmentCoordinates(detail.show, episode);
        if (detail.show.isMovie() && coordinates && !keys.isEmpty()) {
            const auto [number, item] = *coordinates;
            QMap<QString, Stored> saved;
            QList<providers::ImportedSegment> live;
            for (const auto &[identity, value] : latest) {
                const auto &[storedSeason, storedEpisode, key] = identity;
                if (storedSeason != number || storedEpisode != item || !validSegmentKey(key, true)) continue;
                saved[key] = value;
                if (value.deleted) continue;
                row[key] = {value.start, value.end};
                live.append({providers::segmentType(segmentKind(key)), value.start, value.end});
                if (value.source == "video_editor") result.edited.insert(episode.identity.itemId);
            }
            for (const auto &candidate : episode.importedSegments) {
                if (!providers::validMovieSegment(candidate, episode.durationMs.value_or(0) / 1000.)) continue;
                const auto kind = providers::segmentName(candidate.type);
                const auto startMs = qint64(std::round(candidate.startSeconds * 1000));
                auto primary = saved.find(kind);
                if (primary != saved.end() && primary->chapterStart.isNull()) {
                    if (primary->deleted) continue; // Legacy deletions suppressed the entire type.
                    const auto exact = std::find_if(episode.importedSegments.cbegin(), episode.importedSegments.cend(),
                        [&](const auto &chapter) { return chapter.type == candidate.type && chapter.startSeconds == primary->start
                            && chapter.endSeconds == primary->end; });
                    if (exact == episode.importedSegments.cend() || exact->startSeconds == candidate.startSeconds) {
                        // Associate the legacy range with its original chapter before importing siblings.
                        sql::execute(connection.db, "INSERT INTO movie_segment_chapters VALUES(?,?,?)", {keys.first(), kind, startMs});
                        primary->chapterStart = startMs;
                    }
                }
                if (primary != saved.end() && !primary->chapterStart.isNull() && primary->chapterStart.toLongLong() == startMs) continue;
                const auto key = primary == saved.end() ? kind : kind + ":chapter-" + QString::number(startMs);
                if (saved.contains(key) || std::any_of(live.cbegin(), live.cend(), [&](const auto &other) {
                        return providers::movieSegmentsConflict(candidate, other);
                    })) continue;
                sql::execute(connection.db, "INSERT INTO segments "
                    "(show_key,season,episode,kind,start,end,source,provider,provider_episode) VALUES(?,?,?,?,?,?,?,?,?)",
                    {keys.first(), number, item, key, candidate.startSeconds, candidate.endSeconds, "media_server",
                     providers::providerName(episode.identity.provider), episode.identity.itemId});
                if (key == kind) sql::execute(connection.db, "INSERT INTO movie_segment_chapters VALUES(?,?,?)", {keys.first(), kind, startMs});
                saved[key] = {candidate.startSeconds, candidate.endSeconds, false, "media_server", startMs};
                row[key] = {candidate.startSeconds, candidate.endSeconds};
                live.append(candidate);
            }
            const auto reset = resets.find({number, item});
            if (reset != resets.end()) result.statusRevisions[episode.identity.itemId] = reset->second;
            result.values[episode.identity.itemId] = row;
            continue;
        }
        if(!episode.seasonNumber || !result.deferredSeasons.contains(*episode.seasonNumber))
            for (const auto &segment : episode.importedSegments)
                row[providers::segmentName(segment.type)] = {segment.startSeconds, segment.endSeconds};
        if (!keys.isEmpty() && coordinates) {
            const auto [seasonNumber, episodeNumber] = *coordinates;
            const auto reset=resets.find({seasonNumber,episodeNumber});
            if(reset!=resets.end())result.statusRevisions[episode.identity.itemId]=reset->second;
            for (const auto &kind : {QString("recap"), QString("intro"), QString("credits"), QString("preview")}) {
                const auto identity = std::make_tuple(seasonNumber, episodeNumber, kind);
                const auto stored = latest.find(identity);
                if (stored != latest.end()) {
                    if (stored->second.deleted) row.remove(kind);
                    else {
                        row[kind] = {stored->second.start, stored->second.end};
                        if (stored->second.source == "video_editor") result.edited.insert(episode.identity.itemId);
                    }
                } else if (row.contains(kind)) {
                    const auto value = row.value(kind);
                    if (std::isfinite(value.first) && std::isfinite(value.second) && 0 <= value.first && value.first < value.second) {
                        sql::execute(connection.db, "INSERT OR IGNORE INTO segments "
                            "(show_key,season,episode,kind,start,end,source,provider,provider_episode) VALUES(?,?,?,?,?,?,?,?,?)",
                            {keys.first(), seasonNumber, episodeNumber, kind, value.first, value.second,
                             episode.identity.provider == providers::ProviderId::Local ? "embedded" : "media_server",
                             providers::providerName(episode.identity.provider), episode.identity.itemId});
                        latest.emplace(identity, Stored{value.first, value.second, false,
                            episode.identity.provider == providers::ProviderId::Local ? "embedded" : "media_server", {}});
                    }
                }
            }
        }
        result.values[episode.identity.itemId] = row;
    }
    transaction.commit();
    return result;
}

} // namespace introvert::segments
