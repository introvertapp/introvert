#include "catalog_segments.h"
#include "identity.h"
#include "core/sqlite.h"
#include <cmath>
#include <map>
#include <tuple>

namespace introvert::segments {

CatalogSegments loadCatalogSegments(const QString &databasePath, const providers::ShowDetail &detail)
{
    CatalogSegments result;
    const auto keys = showKeys(detail.show);
    struct Stored { double start, end; bool deleted; QString source; };
    std::map<std::tuple<qint64, qint64, QString>, Stored> latest;
    std::map<std::pair<qint64,qint64>,qint64> resets;
    sql::Connection connection(databasePath);
    sql::Transaction transaction(connection.db);
    initializeTables(connection.db);
    if (!keys.isEmpty()) {
        QStringList placeholders;
        QVariantList arguments;
        for (const auto &key : keys) { placeholders.append("?"); arguments.append(key); }
        auto query = sql::execute(connection.db, "SELECT season,episode,kind,start,end,deleted,source FROM segments "
            "WHERE show_key IN (" + placeholders.join(',') + ") ORDER BY revision DESC", arguments);
        while (query.next())
            latest.emplace(std::make_tuple(query.value(0).toLongLong(), query.value(1).toLongLong(), query.value(2).toString()),
                Stored{query.value(3).toDouble(), query.value(4).toDouble(), query.value(5).toBool(), query.value(6).toString()});
        auto cleared=sql::execute(connection.db,"SELECT season,episode,MAX(revision) FROM episode_status_resets WHERE show_key IN ("
            +placeholders.join(',')+") GROUP BY season,episode",arguments);
        while(cleared.next())resets[{cleared.value(0).toLongLong(),cleared.value(1).toLongLong()}]=cleared.value(2).toLongLong();
    }
    for (const auto &season : detail.seasons) for (const auto &episode : season.episodes) {
        SegmentValues row;
        for (const auto &segment : episode.importedSegments)
            row[providers::segmentName(segment.type)] = {segment.startSeconds, segment.endSeconds};
        if (!keys.isEmpty() && episode.seasonNumber && episode.episodeNumber) {
            const auto reset=resets.find({*episode.seasonNumber,*episode.episodeNumber});
            if(reset!=resets.end())result.statusRevisions[episode.identity.itemId]=reset->second;
            for (const auto &kind : {QString("recap"), QString("intro"), QString("credits"), QString("preview")}) {
                const auto identity = std::make_tuple(*episode.seasonNumber, *episode.episodeNumber, kind);
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
                            {keys.first(), *episode.seasonNumber, *episode.episodeNumber, kind, value.first, value.second,
                             episode.identity.provider == providers::ProviderId::Local ? "embedded" : "media_server",
                             providers::providerName(episode.identity.provider), episode.identity.itemId});
                        latest.emplace(identity, Stored{value.first, value.second, false,
                            episode.identity.provider == providers::ProviderId::Local ? "embedded" : "media_server"});
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
