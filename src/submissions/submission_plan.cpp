#include "submission_plan.h"
#include "segments/identity.h"
#include <set>
#include <QElapsedTimer>
#include <QThread>

namespace introvert::submissions {

QVariantMap prepareSubmission(const providers::ShowDetail &detail, const QVariantList &selection,
                              const QString &endpoint, const SettingsStore &settings, const segments::SegmentStore &segments,
                              const providers::Cancellation &cancelled)
{
    if (endpoint != "introdb_app" && endpoint != "theintrodb_org") throw SubmissionError("Choose a submission endpoint");
    const auto client = EndpointClient::fromStore(endpoint, settings);
    if (!client.configured()) throw SubmissionError("Enable the endpoint and save its API key in Settings → Endpoints");
    if (selection.isEmpty()) throw SubmissionError("Select at least one segment");
    QMap<QString, providers::Episode> episodes;
    for (const auto &season : detail.seasons) for (const auto &episode : season.episodes) episodes[episode.identity.itemId] = episode;
    QVariantList items;
    QMap<QString,QVariantMap> resolved;
    QElapsedTimer mappingClock;mappingClock.start();qint64 mappings=0;
    std::set<std::pair<QString, QString>> seen;
    QSet<qint64> seasons;
    for (const auto &value : selection) {
        if (value.metaType().id() != QMetaType::QVariantMap) throw SubmissionError("Invalid episode or segment selection");
        const auto row = value.toMap();
        const auto id = row.value("id");
        const auto kinds = row.value("segmentTypes");
        if (id.metaType().id() != QMetaType::QString || !episodes.contains(id.toString())
            || kinds.metaType().id() != QMetaType::QVariantList || kinds.toList().isEmpty())
            throw SubmissionError("Invalid episode or segment selection");
        for (const auto &kind : kinds.toList())
            if (kind.metaType().id() != QMetaType::QString || !QStringList{"recap", "intro", "credits", "preview"}.contains(kind.toString()))
                throw SubmissionError("Invalid episode or segment selection");
        const auto &episode = episodes[id.toString()];
        const auto current = segments.load(detail.show, episode);
        for (const auto &value : kinds.toList()) {
            const auto kind = value.toString();
            if (!seen.emplace(episode.identity.itemId, kind).second) continue;
            if (!current.contains(kind)) throw SubmissionError("Selected timestamps are no longer available. Review the selection again.");
            const auto times = current.value(kind);
            const auto duration = segments.durationMs(episode);
            providers::checkCancelled(cancelled);
            QVariantMap values{{"tvdb_id", detail.show.externalIds.tvdb ? QVariant(*detail.show.externalIds.tvdb) : QVariant()},
                {"numbering_source", episode.numberingSource.value_or(QString())}, {"mapping", episode.episodeOrder.value_or(QString())},
                {"tvdb_episode_id", episode.tvdbEpisodeId ? QVariant(*episode.tvdbEpisodeId) : QVariant()},
                {"episode_name", episode.title.value_or(QString())}, {"imdb_id", detail.show.externalIds.imdb ? QVariant(*detail.show.externalIds.imdb) : QVariant()},
                {"tmdb_id", detail.show.externalIds.tmdb ? QVariant(*detail.show.externalIds.tmdb) : QVariant()},
                {"season", episode.seasonNumber ? QVariant(*episode.seasonNumber) : QVariant()},
                {"episode", episode.episodeNumber ? QVariant(*episode.episodeNumber) : QVariant()}, {"segment_type", kind},
                {"start_sec", times.first}, {"end_sec", times.second}, {"video_duration_ms", duration ? QVariant(*duration) : QVariant()}};
            if (endpoint == "theintrodb_org") {
                if (!episode.numberingSource && !episode.tvdbEpisodeId)
                    throw SubmissionError("Episode numbering source is unknown. Refresh metadata and confirm its episode order before submitting.");
                if (!resolved.contains(episode.identity.itemId)) {
                    if(!episode.tvdbEpisodeId || !detail.show.externalIds.tvdb) {
                        while(mappingClock.elapsed()<mappings*260) {providers::checkCancelled(cancelled);QThread::msleep(10);}
                        ++mappings;
                    }
                    const auto verified = client.resolve(values,cancelled);
                    QVariantMap address;
                    for (const auto &name : {"tvdb_id","tmdb_id","imdb_id","numbering_source","mapping","tvdb_episode_id",
                                             "target_season","target_episode","episode_name","resolved_title"})
                        if (verified.contains(name)) address[name] = verified[name];
                    resolved[episode.identity.itemId] = address;
                }
                const auto address = resolved.value(episode.identity.itemId);
                for (auto i=address.begin();i!=address.end();++i) values[i.key()] = i.value();
            }
            client.validate(values);
            values["episode_id"] = episode.identity.itemId; values["title"] = episode.title.value_or(QString());
            items.append(values); seasons.insert(*episode.seasonNumber);
        }
    }
    if (seasons.size() != 1) throw SubmissionError("Select episodes from one season");
    return {{"endpoint", endpoint}, {"provider", providers::providerName(detail.show.identity.provider)}, {"show_id", detail.show.identity.itemId},
        {"show", detail.show.title}, {"imdb", detail.show.externalIds.imdb ? QVariant(*detail.show.externalIds.imdb) : QVariant()},
        {"tmdb", detail.show.externalIds.tmdb ? QVariant(*detail.show.externalIds.tmdb) : QVariant()},
        {"tvdb", detail.show.externalIds.tvdb ? QVariant(*detail.show.externalIds.tvdb) : QVariant()},
        {"canonical", segments::showKeys(detail.show)}, {"season", items.first().toMap().value("season")}, {"items", items}};
}

} // namespace introvert::submissions
