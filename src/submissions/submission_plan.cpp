#include "submission_plan.h"
#include "core/endpoints.h"
#include "segments/identity.h"
#include <set>
#include <QElapsedTimer>
#include <QThread>

namespace introvert::submissions {

QVariantMap prepareSubmission(const providers::ShowDetail &detail, const QVariantList &selection,
                              const QString &endpoint, const SettingsStore &settings, const segments::SegmentStore &segments,
                              const providers::Cancellation &cancelled, const QString &scope, bool allowMissingMovieCredits)
{
    if (!endpointById(endpoint)) throw SubmissionError("Choose a submission endpoint");
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
    const bool movie = detail.show.isMovie();
    const bool movieCreditsOnly = movie && endpoint == "introdb_app";
    bool excludedMovieRanges = false;
    for (const auto &value : selection) {
        providers::checkCancelled(cancelled);
        if (value.metaType().id() != QMetaType::QVariantMap) throw SubmissionError("Invalid episode or segment selection");
        const auto row = value.toMap();
        const auto id = row.value("id");
        const auto kinds = row.value("segmentTypes");
        if (id.metaType().id() != QMetaType::QString || !episodes.contains(id.toString())
            || kinds.metaType().id() != QMetaType::QVariantList || kinds.toList().isEmpty())
            throw SubmissionError("Invalid episode or segment selection");
        for (const auto &key : kinds.toList())
            if (key.metaType().id() != QMetaType::QString || !segments::validSegmentKey(key.toString(), movie))
                throw SubmissionError("Invalid episode or segment selection");
        const auto &episode = episodes[id.toString()];
        const auto coordinates = segments::segmentCoordinates(detail.show, episode);
        if (!coordinates) throw SubmissionError("Invalid episode selection");
        const auto current = segments.load(detail.show, episode);
        for (const auto &value : kinds.toList()) {
            const auto key = value.toString(), kind = segments::segmentKind(key);
            if (!seen.emplace(episode.identity.itemId, key).second) continue;
            if (movieCreditsOnly && kind != "credits") {
                excludedMovieRanges = true;
                continue;
            }
            if (!current.contains(key)) throw SubmissionError("Selected timestamps are no longer available. Review the selection again.");
            const auto times = current.value(key);
            if (movieCreditsOnly && !items.isEmpty())
                throw SubmissionError("Select one Credits range for IntroDB, or choose TheIntroDB for multiple ranges.");
            if (movie && endpoint == "skipdb_tv") for (const auto &selected : items) {
                if (selected.toMap().value("segment_type") == kind)
                    throw SubmissionError("Select one movie range per segment type for SkipDB.");
            }
            if (movie && endpoint == "theintrodb_org") for (const auto &selected : items) {
                const auto previous = selected.toMap();
                const auto previousKind = previous.value("segment_type").toString();
                if (providers::movieSegmentsConflict(
                        {providers::segmentType(kind), times.first, times.second},
                        {providers::segmentType(previousKind), previous.value("start_sec").toDouble(), previous.value("end_sec").toDouble()}))
                    throw SubmissionError("Movie segments cannot overlap. Leave at least 5 seconds between segments of the same type.");
            }
            const auto duration = segments.durationMs(episode, &detail.show);
            providers::checkCancelled(cancelled);
            QVariantMap values{{"tvdb_id", detail.show.externalIds.tvdb ? QVariant(*detail.show.externalIds.tvdb) : QVariant()},
                {"numbering_source", episode.numberingSource.value_or(QString())}, {"mapping", episode.episodeOrder.value_or(QString())},
                {"tvdb_episode_id", episode.tvdbEpisodeId ? QVariant(*episode.tvdbEpisodeId) : QVariant()},
                {"episode_name", episode.title.value_or(QString())}, {"imdb_id", detail.show.externalIds.imdb ? QVariant(*detail.show.externalIds.imdb) : QVariant()},
                {"tmdb_id", detail.show.externalIds.tmdb ? QVariant(*detail.show.externalIds.tmdb) : QVariant()},
                {"season", episode.seasonNumber ? QVariant(*episode.seasonNumber) : QVariant()},
                {"episode", episode.episodeNumber ? QVariant(*episode.episodeNumber) : QVariant()}, {"segment_type", kind},
                {"start_sec", times.first}, {"end_sec", times.second}, {"video_duration_ms", duration ? QVariant(*duration) : QVariant()}};
            if (movie) {
                values["media_type"] = "movie";
                values["segment_key"] = key;
                values["season"] = coordinates->first;
                values["episode"] = coordinates->second;
            }
            if (endpoint == "theintrodb_org") {
                if (!movie && !episode.numberingSource && !episode.tvdbEpisodeId)
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
            items.append(values); seasons.insert(coordinates->first);
        }
    }
    if (movieCreditsOnly && items.isEmpty()) {
        if (allowMissingMovieCredits) return {};
        throw SubmissionError("Select one movie Credits range for IntroDB, or choose TheIntroDB for other movie segments.");
    }
    if (seasons.size() != 1) throw SubmissionError("Select episodes from one season");
    const auto saved = settings.loadProvider(providers::providerName(detail.show.identity.provider));
    const auto sourceScope = scope.isEmpty() ? providers::providerScope(detail.show.identity,
        detail.show.localRoot.value_or(saved.provider == "local" ? saved.localRoot : saved.remoteUrl)) : scope;
    QVariantMap plan{{"scope", sourceScope}, {"endpoint", endpoint}, {"provider", providers::providerName(detail.show.identity.provider)}, {"show_id", detail.show.identity.itemId},
        {"show", detail.show.title}, {"imdb", detail.show.externalIds.imdb ? QVariant(*detail.show.externalIds.imdb) : QVariant()},
        {"tmdb", detail.show.externalIds.tmdb ? QVariant(*detail.show.externalIds.tmdb) : QVariant()},
        {"tvdb", detail.show.externalIds.tvdb ? QVariant(*detail.show.externalIds.tvdb) : QVariant()},
        {"canonical", segments::showKeys(detail.show)}, {"season", items.first().toMap().value("season")}, {"items", items}};
    if (movie) plan["movie"] = true;
    if (excludedMovieRanges) plan["excludedMovieRanges"] = true;
    return plan;
}

} // namespace introvert::submissions
