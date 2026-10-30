#include "emby_provider.h"
#include "http_client.h"
#include "json_values.h"

#include <QJsonArray>
#include <QMap>
#include <QSet>
#include <algorithm>
#include <cmath>

namespace introvert::providers {
namespace {

QByteArray jsonHeaderString(const QString &value)
{
    // Match json.dumps(..., ensure_ascii=True), including UTF-16 surrogate pairs.
    const auto quoted = QJsonValue(value).toJson(QJsonValue::JsonFormat::Compact);
    QByteArray result;
    for (const auto c : QString::fromUtf8(quoted)) {
        if (c.unicode() > 126) result.append("\\u" + QByteArray::number(c.unicode(), 16).rightJustified(4, '0'));
        else result.append(char(c.unicode()));
    }
    return result;
}

std::optional<ImportedSegment> extract(const QJsonArray &chapters, const QStringList &starts,
                                      const QStringList &ends, SegmentType kind)
{
    std::optional<double> start, end;
    for (const auto &value : chapters) {
        const auto chapter = value.toObject();
        const auto ticks = detail::finiteNumber(chapter["StartPositionTicks"], true);
        if (!ticks) continue;
        const auto seconds = std::floor(*ticks / 10000000);
        const auto name = detail::sourceString(chapter["MarkerType"]);
        if (starts.contains(name)) start = seconds;
        if (ends.contains(name)) end = seconds;
    }
    if (start && end) return ImportedSegment{kind, *start, *end};
    return std::nullopt;
}

QList<ImportedSegment> chapters(const QJsonArray &values, double duration)
{
    QList<ImportedSegment> result;
    const auto intro = extract(values, {"IntroStart", "1"}, {"IntroEnd", "2"}, SegmentType::Intro);
    if (intro) result.append(*intro);
    auto credits = extract(values, {"CreditsStart", "3"}, {"CreditsEnd", "4"}, SegmentType::Credits);
    if (!credits) credits = extract(values, {"OutroStart", "3"}, {"OutroEnd", "4"}, SegmentType::Credits);
    // Emby's native credits marker supplies only a start. Its interval ends at
    // the declared episode end when no explicit end marker is available.
    if (!credits && duration > 0) for (const auto &value : values) {
        const auto chapter = value.toObject();
        const auto name = detail::sourceString(chapter["MarkerType"]);
        const auto ticks = detail::finiteNumber(chapter["StartPositionTicks"]);
        if (QStringList{"CreditsStart", "OutroStart", "3"}.contains(name) && ticks && *ticks >= 0 && *ticks / 10000000 < duration)
            credits = ImportedSegment{SegmentType::Credits, *ticks / 10000000, duration};
    }
    if (credits) result.append(*credits);
    for (const auto kind : {SegmentType::Recap, SegmentType::Preview}) {
        auto name = segmentName(kind); name[0] = name[0].toUpper();
        const auto segment = extract(values, {name + "Start"}, {name + "End"}, kind);
        if (segment) result.append(*segment);
    }
    return result;
}

QList<ImportedSegment> mediaSegments(const QJsonValue &response)
{
    if (!response.isObject() || !response.toObject()["Items"].isArray())
        throw ProviderError("Jellyfin returned an invalid media segment list.");
    QList<ImportedSegment> result;
    QSet<QString> seen;
    for (const auto &value : response.toObject()["Items"].toArray()) {
        const auto item = value.toObject();
        auto kind = item["Type"].toString().toLower();
        if (kind == "outro") kind = "credits";
        if (!QStringList{"recap", "intro", "credits", "preview"}.contains(kind) || seen.contains(kind)) continue;
        if (item["StartTicks"].isBool() || item["EndTicks"].isBool()) continue;
        const auto start = detail::finiteNumber(item["StartTicks"]), end = detail::finiteNumber(item["EndTicks"]);
        if (!start || !end || *start < 0 || *start >= *end) continue;
        result.append({segmentType(kind), *start / 10000000, *end / 10000000}); seen.insert(kind);
    }
    return result;
}

} // namespace

EmbyProvider::EmbyProvider() : EmbyProvider(ProviderId::Emby) {}
EmbyProvider::EmbyProvider(ProviderId provider)
    : m_provider(provider), m_label(provider == ProviderId::Emby ? "Emby" : "Jellyfin"),
      m_prefix(provider == ProviderId::Emby ? "/emby" : "")
{
}

QMap<QByteArray, QByteArray> EmbyProvider::headers(const ProviderConfig &config) const
{
    if (config.provider != m_provider)
        throw ProviderError(QStringLiteral("The saved media provider does not match this request."));
    const auto key = config.credential.value_or(QString());
    return m_provider == ProviderId::Jellyfin
        ? QMap<QByteArray, QByteArray>{{"Authorization", "MediaBrowser Token=" + jsonHeaderString(key)}}
        : QMap<QByteArray, QByteArray>{{"X-Emby-Token", key.toUtf8()}};
}

QJsonValue EmbyProvider::request(const ProviderConfig &config, const QString &path,
                                const QVariantMap &parameters, const Cancellation &cancelled)
{
    return HttpClient::json(config, path, parameters, m_label, headers(config), cancelled);
}

void EmbyProvider::invalidate()
{
    m_userAge.invalidate(); m_userUrl.reset(); m_userCredential.reset(); m_user.reset();
}

std::optional<QString> EmbyProvider::user(const ProviderConfig &config, const Cancellation &cancelled)
{
    checkCancelled(cancelled);
    if (m_provider != ProviderId::Jellyfin) return std::nullopt;
    if (m_userAge.isValid() && m_userAge.elapsed() < 300000
        && m_userUrl == config.remoteUrl && m_userCredential == config.credential) return m_user;
    const auto values = request(config, "/Users", {}, cancelled);
    if (!values.isArray()) throw ProviderError(QStringLiteral("Jellyfin returned an invalid user list."));
    const auto users = values.toArray();
    std::optional<QString> selected;
    if (!users.isEmpty()) {
        if (!users.first().isObject()) throw ProviderError(QStringLiteral("Jellyfin returned an invalid user list."));
        const auto id = users.first().toObject()["Id"];
        if (detail::present(id) && !(id.isString() && id.toString().isEmpty())
            && !(id.isDouble() && id.toDouble() == 0) && !(id.isBool() && !id.toBool())) selected = detail::sourceString(id);
    }
    m_userUrl = config.remoteUrl; m_userCredential = config.credential; m_user = selected; m_userAge.start();
    return selected;
}

QList<QJsonObject> EmbyProvider::items(const ProviderConfig &config, const QString &path,
                                      const QVariantMap &parameters, const Cancellation &cancelled)
{
    QList<QJsonObject> rows;
    qint64 offset = 0;
    while (true) {
        auto query = parameters; query["StartIndex"] = offset; query["Limit"] = 500;
        const auto value = request(config, path, query, cancelled);
        if (!value.isObject()) throw ProviderError(m_label + " returned an invalid item list.");
        const auto result = value.toObject();
        const auto list = result.contains("Items") ? result["Items"] : QJsonValue(QJsonArray{});
        if (!list.isArray()) throw ProviderError(m_label + " returned an invalid item list.");
        const auto page = list.toArray();
        for (const auto &item : page) if (item.isObject()) rows.append(item.toObject());
        offset += page.size();
        const auto total = detail::nonnegativeNumber(result["TotalRecordCount"]);
        if (!total || offset >= *total || page.isEmpty()) return rows;
    }
}

QString EmbyProvider::web(const ProviderConfig &config, const QString &key, const std::optional<QString> &server) const
{
    const auto route = m_provider == ProviderId::Emby ? "item" : "details";
    return detail::baseUrl(config.remoteUrl.value_or(QString())) + "/web/index.html#!/" + route
        + "?id=" + detail::quote(key) + (server && !server->isEmpty() ? "&serverId=" + detail::quote(*server) : QString());
}

ShowSummary EmbyProvider::show(const ProviderConfig &config, const QJsonObject &item, bool includeImages) const
{
    if (!detail::present(item["Id"])) throw ProviderError(m_label + " returned an invalid media response.");
    const auto key = detail::sourceString(item["Id"]);
    const auto server = detail::optionalString(item["ServerId"]);
    ShowSummary result{{m_provider, key, server}, item["Name"].toString()};
    result.year = detail::nonnegativeNumber(item["ProductionYear"]);
    QMap<QString, QString> ids;
    const auto values = item["ProviderIds"].toObject();
    for (auto it = values.begin(); it != values.end(); ++it) if (detail::present(it.value())) ids[it.key().toLower()] = detail::sourceString(it.value());
    auto external = [&](const QString &name) -> std::optional<QString> { return ids.contains(name) ? std::optional<QString>(ids[name]) : std::nullopt; };
    result.externalIds = {external("imdb"), external("tmdb"), external("tvdb"), external("zap2it"), external("tvmaze")};
    if (includeImages) {
        result.posterUrl = m_prefix + "/Items/" + detail::quote(key) + "/Images/Primary";
        result.logoUrl = m_prefix + "/Items/" + detail::quote(key) + "/Images/Logo";
    }
    result.itemUrl = web(config, key, server);
    result.seasonCount = detail::nonnegativeNumber(item["ChildCount"], m_provider == ProviderId::Emby);
    result.episodeCount = detail::nonnegativeNumber(item["RecursiveItemCount"], m_provider == ProviderId::Emby);
    return result;
}

QList<ShowSummary> EmbyProvider::listSeries(const ProviderConfig &config, bool includeImages, const Cancellation &cancelled)
{
    const auto selected = user(config, cancelled);
    if (m_provider == ProviderId::Jellyfin && !selected) return {};
    const auto path = m_prefix + (selected ? "/Users/" + detail::quote(*selected) : QString()) + "/Items";
    QString fields = QStringLiteral("Name,ProductionYear,ChildCount,RecursiveItemCount");
    if (m_provider == ProviderId::Jellyfin) fields += ",ProviderIds";
    if (includeImages) fields += ",ImageTags";
    const auto values = items(config, path, {{"Recursive", true}, {"IncludeItemTypes", "Series"},
                                            {"Fields", fields}, {"EnableImages", includeImages}}, cancelled);
    QSet<QString> seen;
    QList<ShowSummary> result;
    for (const auto &item : values) {
        if (!detail::present(item["Id"])) continue;
        const auto key = detail::sourceString(item["Id"]);
        if (seen.contains(key)) continue;
        seen.insert(key); result.append(show(config, item, includeImages));
    }
    std::stable_sort(result.begin(), result.end(), [](const auto &a, const auto &b) { return a.title.toCaseFolded() < b.title.toCaseFolded(); });
    return result;
}

std::optional<ShowDetail> EmbyProvider::getShow(const ProviderConfig &config, const QString &showId,
                                               bool includeImages, const Cancellation &cancelled)
{
    const auto selected = user(config, cancelled);
    if (m_provider == ProviderId::Jellyfin && !selected) return std::nullopt;
    const auto metadata = items(config, m_prefix + "/Items", {{"Ids", showId},
        {"Fields", "ProviderIds,Name,ProductionYear,ChildCount,RecursiveItemCount"}}, cancelled);
    if (metadata.isEmpty()) return std::nullopt;
    ShowDetail result{show(config, metadata.first(), includeImages), {}};
    const auto path = m_prefix + (selected ? "/Users/" + detail::quote(*selected) : QString()) + "/Items";
    QVariantMap params{{"ParentId", showId}, {"IncludeItemTypes", "Episode"}, {"Recursive", true},
        {"Fields", "Name,IndexNumber,ParentIndexNumber,Chapters,ProviderIds,Path,MediaSources,RunTimeTicks"}};
    if (m_provider == ProviderId::Jellyfin) { params["SortBy"] = "ParentIndexNumber,IndexNumber"; params["SortOrder"] = "Ascending"; }
    else { params["EnableUserData"] = true; params["EnableImages"] = includeImages; }
    QMap<qint64, QList<Episode>> seasons;
    QSet<QString> seen;
    for (const auto &item : items(config, path, params, cancelled)) {
        if (!detail::present(item["Id"])) continue;
        const auto key = detail::sourceString(item["Id"]);
        if (seen.contains(key)) continue;
        seen.insert(key);
        const auto season = detail::nonnegativeNumber(item["ParentIndexNumber"]).value_or(0);
        auto server = detail::optionalString(item["ServerId"]);
        if (!server || server->isEmpty()) server = result.show.identity.serverId;
        Episode episode{{m_provider, key, server}};
        episode.title = detail::optionalString(item["Name"]); episode.seasonNumber = season;
        episode.episodeNumber = detail::nonnegativeNumber(item["IndexNumber"]).value_or(0);
        const auto episodeIds=item["ProviderIds"].toObject();
        for(const auto &name:episodeIds.keys()) {
            if(name.compare("tvdb",Qt::CaseInsensitive)==0 && detail::present(episodeIds[name])) {
                episode.tvdbEpisodeId=detail::sourceString(episodeIds[name]);episode.numberingSource="tvdb";
            }
        }
        const auto ordering=normalizedEpisodeOrder(metadata.first()["DisplayOrder"].toString());
        if(!ordering.isEmpty()) {episode.episodeOrder=ordering;if(result.show.externalIds.tvdb)episode.numberingSource="tvdb";}
        episode.itemUrl = web(config, key, server);
        episode.mediaPath = detail::optionalString(item["Path"]);
        if (!episode.mediaPath || episode.mediaPath->isEmpty()) {
            const auto media = item["MediaSources"].toArray();
            if (!media.isEmpty()) episode.mediaPath = detail::optionalString(media.first().toObject()["Path"]);
        }
        const auto duration = detail::nonnegativeNumber(item["RunTimeTicks"]).value_or(0) / 10000;
        if (duration) episode.durationMs = duration;
        episode.importedSegments = chapters(item["Chapters"].toArray(), duration / 1000.);
        if (m_provider == ProviderId::Jellyfin) {
            try {
                const auto segments = mediaSegments(request(config, "/MediaSegments/" + detail::quote(key), {}, cancelled));
                for (const auto &segment : segments) {
                    if (duration && segment.endSeconds > duration / 1000.) continue;
                    episode.importedSegments.removeIf([&](const auto &old) { return old.type == segment.type; });
                    episode.importedSegments.append(segment);
                }
            } catch (const ProviderError &error) {
                // Older servers do not expose this optional endpoint. Other
                // failures must remain retryable instead of caching no markers.
                if (error.httpStatus() != 404 && error.httpStatus() != 405) throw;
            }
        }
        seasons[season].append(episode);
    }
    if (!result.show.seasonCount) result.show.seasonCount = seasons.size();
    if (!result.show.episodeCount) result.show.episodeCount = seen.size();
    for (auto season = seasons.begin(); season != seasons.end(); ++season) {
        std::stable_sort(season->begin(), season->end(), [](const auto &a, const auto &b) { return a.episodeNumber < b.episodeNumber; });
        result.seasons.append({season.key(), *season});
    }
    return result;
}

QByteArray EmbyProvider::artwork(const ProviderConfig &config, const QString &path, const Cancellation &cancelled)
{
    return HttpClient::image(config, path, m_label, headers(config), cancelled);
}

} // namespace introvert::providers
