#include "plex_provider.h"
#include "http_client.h"
#include "json_values.h"

#include <QJsonArray>
#include <QMap>
#include <QSet>
#include <QUrl>
#include <algorithm>
#include <cmath>

namespace introvert::providers {
namespace {

QMap<QByteArray, QByteArray> headers(const ProviderConfig &config)
{
    return {{"X-Plex-Token", config.credential.value_or(QString()).toUtf8()}, {"X-Plex-Product", "Introvert"}};
}

QList<ImportedSegment> markers(const QJsonArray &values)
{
    QList<ImportedSegment> result;
    for (const auto kind : {SegmentType::Intro, SegmentType::Credits}) {
        const QStringList names = kind == SegmentType::Intro ? QStringList{"intro"} : QStringList{"credits", "outro"};
        bool found = false;
        for (const auto &name : names) {
            for (const auto &value : values) {
                const auto marker = value.toObject();
                if (detail::sourceString(marker["type"]).toLower() != name) continue;
                const auto start = detail::finiteNumber(marker["startTimeOffset"]);
                const auto end = detail::finiteNumber(marker["endTimeOffset"]);
                if (start && end && *start >= 0 && *start < *end) {
                    result.append({kind, std::floor(*start / 1000), std::floor(*end / 1000)});
                    found = true; break;
                }
            }
            if (found) break;
        }
    }
    return result;
}

} // namespace

void PlexProvider::check(const ProviderConfig &config, const Cancellation &cancelled) const
{
    checkCancelled(cancelled);
    if (config.provider != ProviderId::Plex || !config.remoteUrl || config.remoteUrl->isEmpty())
        throw ProviderError(QStringLiteral("Enter a Plex server URL in Settings → Media."));
    const QUrl url(*config.remoteUrl, QUrl::StrictMode);
    if (!url.isValid() || (url.scheme() != "http" && url.scheme() != "https") || url.host().isEmpty()
        || url.authority().contains('@') || !url.query().isEmpty() || !url.fragment().isEmpty())
        throw ProviderError(QStringLiteral("Enter a valid Plex HTTP/HTTPS server URL without credentials in the URL."));
    if (!config.credential || config.credential->isEmpty())
        throw ProviderError(QStringLiteral("Enter your Plex API key/token in Settings → Media and save it."));
}

QJsonValue PlexProvider::request(const ProviderConfig &config, const QString &path, const QVariantMap &parameters,
                                 const Cancellation &cancelled)
{
    check(config, cancelled);
    const auto result = HttpClient::json(config, path, parameters, QStringLiteral("Plex"), headers(config), cancelled);
    if (!result.isObject() || !result.toObject()["MediaContainer"].isObject())
        throw ProviderError(QStringLiteral("Plex returned an invalid media response."));
    return result.toObject()["MediaContainer"];
}

void PlexProvider::context(const ProviderConfig &config, const Cancellation &cancelled)
{
    check(config, cancelled);
    if (m_contextAge.isValid() && m_contextAge.elapsed() < 300000
        && m_contextUrl == config.remoteUrl && m_contextCredential == config.credential) return;
    const auto machine = detail::optionalString(request(config, "/identity", {}, cancelled).toObject()["machineIdentifier"]);
    const auto directories = request(config, "/library/sections", {}, cancelled).toObject()["Directory"].toArray();
    QStringList sections;
    for (const auto &directory : directories) {
        if (!directory.isObject()) continue;
        const auto item = directory.toObject();
        if (detail::present(item["key"]) && item["type"] == "show") sections.append(detail::sourceString(item["key"]));
    }
    m_contextUrl = config.remoteUrl; m_contextCredential = config.credential;
    m_machine = machine; m_sections = sections; m_contextAge.start();
}

void PlexProvider::invalidate()
{
    m_contextAge.invalidate(); m_contextUrl.reset(); m_contextCredential.reset(); m_machine.reset(); m_sections.clear();
}

QList<QJsonObject> PlexProvider::items(const ProviderConfig &config, const QString &path,
                                      const QVariantMap &parameters, const Cancellation &cancelled)
{
    QList<QJsonObject> result;
    qint64 offset = 0;
    while (true) {
        auto query = parameters;
        query["X-Plex-Container-Start"] = offset; query["X-Plex-Container-Size"] = 500;
        const auto container = request(config, path, query, cancelled).toObject();
        const auto values = container.contains("Metadata") ? container.value("Metadata") : QJsonValue(QJsonArray{});
        if (!values.isArray()) throw ProviderError(QStringLiteral("Plex returned an invalid item list."));
        const auto page = values.toArray();
        for (const auto &value : page) if (value.isObject()) result.append(value.toObject());
        const auto total = detail::nonnegativeNumber(container["totalSize"]);
        offset += page.size();
        if (!total || offset >= *total || page.isEmpty()) return result;
        const auto returnedOffset = detail::nonnegativeNumber(container["offset"]);
        if (returnedOffset && *returnedOffset != offset - page.size())
            throw ProviderError(QStringLiteral("Plex returned inconsistent pagination. Try again."));
    }
}

QString PlexProvider::web(const ProviderConfig &config, const QString &key) const
{
    if (!m_machine || m_machine->isEmpty()) return {};
    return detail::baseUrl(config.remoteUrl.value_or(QString())) + "/web/index.html#!/server/"
        + detail::quote(*m_machine) + "/details?key=" + detail::quote("/library/metadata/" + key);
}

ShowSummary PlexProvider::show(const ProviderConfig &config, const QJsonObject &item, bool includeImages) const
{
    if (!detail::present(item["ratingKey"])) throw ProviderError(QStringLiteral("Plex returned an invalid media response."));
    const auto key = detail::sourceString(item["ratingKey"]);
    ShowSummary result{{ProviderId::Plex, key, m_machine}, item["title"].toString()};
    result.year = detail::nonnegativeNumber(item["year"]);
    for (const auto &value : item["Guid"].toArray()) {
        const auto id = value.toObject()["id"].toString();
        if (id.startsWith("imdb://")) result.externalIds.imdb = id.mid(7);
        else if (id.startsWith("tmdb://")) result.externalIds.tmdb = id.mid(7);
        else if (id.startsWith("tvdb://")) result.externalIds.tvdb = id.mid(7);
    }
    if (includeImages) {
        result.posterUrl = detail::optionalString(item["thumb"]);
        for (const auto &image : item["Image"].toArray()) {
            if (image.toObject()["type"] == "clearLogo") { result.logoUrl = detail::optionalString(image.toObject()["url"]); break; }
        }
    }
    if (m_machine && !m_machine->isEmpty()) result.itemUrl = web(config, key);
    result.seasonCount = detail::nonnegativeNumber(item["childCount"]);
    result.episodeCount = detail::nonnegativeNumber(item["leafCount"]);
    return result;
}

QList<ShowSummary> PlexProvider::listSeries(const ProviderConfig &config, bool includeImages, const Cancellation &cancelled)
{
    context(config, cancelled);
    QList<ShowSummary> result;
    QSet<QString> seen;
    for (const auto &section : m_sections) {
        for (const auto &item : items(config, "/library/sections/" + detail::quote(section) + "/all",
                                      {{"type", 2}, {"includeGuids", 1}}, cancelled)) {
            if (!detail::present(item["ratingKey"])) continue;
            const auto key = detail::sourceString(item["ratingKey"]);
            if (!seen.contains(key)) { seen.insert(key); result.append(show(config, item, includeImages)); }
        }
    }
    std::stable_sort(result.begin(), result.end(), [](const auto &a, const auto &b) { return a.title.toCaseFolded() < b.title.toCaseFolded(); });
    return result;
}

std::optional<ShowDetail> PlexProvider::getShow(const ProviderConfig &config, const QString &showId,
                                               bool includeImages, const Cancellation &cancelled)
{
    context(config, cancelled);
    const auto path = "/library/metadata/" + detail::quote(showId);
    const auto metadata = items(config, path, {{"includeGuids", 1}}, cancelled);
    if (metadata.isEmpty()) return std::nullopt;
    QList<QJsonObject> episodes;
    try { episodes = items(config, path + "/allLeaves", {{"includeMarkers", 1}, {"includeGuids", 1}}, cancelled); }
    catch (const ProviderError &) { /* Preserve the source's season-children fallback. */ }
    if (episodes.isEmpty()) {
        for (const auto &season : items(config, path + "/children", {}, cancelled)) {
            if (season["type"] != "season" || !detail::present(season["ratingKey"])) continue;
            const auto seasonPath = "/library/metadata/" + detail::quote(detail::sourceString(season["ratingKey"])) + "/children";
            for (auto episode : items(config, seasonPath, {{"includeMarkers", 1}, {"includeGuids", 1}}, cancelled)) {
                if (!episode.contains("parentIndex")) episode["parentIndex"] = season.contains("index") ? season.value("index") : QJsonValue(0);
                episodes.append(episode);
            }
        }
    }
    QMap<qint64, QList<Episode>> groups;
    QSet<QString> seen;
    for (const auto &item : episodes) {
        if ((item.contains("type") && item["type"] != "episode") || !detail::present(item["ratingKey"])) continue;
        const auto key = detail::sourceString(item["ratingKey"]);
        if (seen.contains(key)) continue;
        seen.insert(key);
        const auto season = detail::nonnegativeNumber(item["parentIndex"]).value_or(0);
        Episode episode{{ProviderId::Plex, key, m_machine}};
        episode.title = detail::optionalString(item["title"]);
        episode.seasonNumber = season; episode.episodeNumber = detail::nonnegativeNumber(item["index"]).value_or(0);
        if (m_machine && !m_machine->isEmpty()) episode.itemUrl = web(config, key);
        for (const auto &media : item["Media"].toArray()) {
            for (const auto &part : media.toObject()["Part"].toArray()) {
                const auto file = part.toObject()["file"].toString();
                if (!file.isEmpty()) { episode.mediaPath = file; break; }
            }
            if (episode.mediaPath) break;
        }
        episode.importedSegments = markers(item["Marker"].toArray());
        episode.durationMs = detail::nonnegativeNumber(item["duration"]);
        groups[season].append(episode);
    }
    ShowDetail result{show(config, metadata.first(), includeImages), {}};
    if (!result.show.seasonCount) result.show.seasonCount = groups.size();
    if (!result.show.episodeCount) result.show.episodeCount = seen.size();
    for (auto group = groups.begin(); group != groups.end(); ++group) {
        std::stable_sort(group->begin(), group->end(), [](const auto &a, const auto &b) { return a.episodeNumber < b.episodeNumber; });
        result.seasons.append({group.key(), *group});
    }
    return result;
}

QByteArray PlexProvider::artwork(const ProviderConfig &config, const QString &path, const Cancellation &cancelled)
{
    check(config, cancelled);
    return HttpClient::image(config, path, QStringLiteral("Plex"), headers(config), cancelled);
}

} // namespace introvert::providers
