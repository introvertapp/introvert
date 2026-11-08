#include "cache_records.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QUrl>
#include <cmath>
#include <limits>

namespace introvert::library {
using namespace providers;
namespace {

void require(bool value) { if (!value) throw std::invalid_argument("Invalid cached media record"); }
QJsonValue json(const std::optional<QString> &value) { return value ? QJsonValue(*value) : QJsonValue(QJsonValue::Null); }
QJsonValue json(const std::optional<qint64> &value) { return value ? QJsonValue(*value) : QJsonValue(QJsonValue::Null); }

QJsonValue artwork(const std::optional<QString> &value)
{
    if (!value || !value->startsWith('/') || value->startsWith("//") || value->contains('\\')) return QJsonValue::Null;
    const QUrl url(*value, QUrl::StrictMode);
    if (!url.isValid() || !url.scheme().isEmpty() || !url.authority().isEmpty() || !url.query().isEmpty() || !url.fragment().isEmpty())
        return QJsonValue::Null;
    return *value;
}
QJsonObject identity(const ProviderIdentity &value)
{
    return {{"provider_id", providerName(value.provider)}, {"item_id", value.itemId}, {"server_id", json(value.serverId)}};
}
template<typename Summary>
QJsonObject title(const Summary &value)
{
    return {{"identity", identity(value.identity)}, {"title", value.title}, {"year", json(value.year)},
        {"poster_url", artwork(value.posterUrl)},
        {"external_ids", QJsonObject{{"imdb", json(value.externalIds.imdb)}, {"tmdb", json(value.externalIds.tmdb)},
            {"tvdb", json(value.externalIds.tvdb)}, {"zap2it", json(value.externalIds.zap2it)}, {"tvmaze", json(value.externalIds.tvmaze)}}}};
}
QJsonObject show(const ShowSummary &value)
{
    auto row = title(value);
    row["logo_url"] = artwork(value.logoUrl);
    row["season_count"] = json(value.seasonCount);
    row["episode_count"] = json(value.episodeCount);
    return row;
}
QJsonObject movie(const MovieSummary &value)
{
    auto row = title(value);
    row["logo_url"] = artwork(value.logoUrl);
    row["duration_ms"] = json(value.durationMs);
    return row;
}
QJsonObject object(const QJsonValue &value) { require(value.isObject()); return value.toObject(); }
QJsonArray array(const QJsonValue &value) { require(value.isArray()); return value.toArray(); }
QString string(const QJsonValue &value) { require(value.isString()); return value.toString(); }
std::optional<QString> optionalString(const QJsonValue &value)
{
    require(!value.isUndefined()); return value.isNull() ? std::nullopt : std::optional<QString>(string(value));
}
std::optional<qint64> integer(const QJsonValue &value)
{
    require(!value.isUndefined());
    if (value.isNull()) return std::nullopt;
    require(value.isDouble() && std::isfinite(value.toDouble()) && std::floor(value.toDouble()) == value.toDouble()
        && value.toDouble() < double(std::numeric_limits<qint64>::max())
        && value.toDouble() >= double(std::numeric_limits<qint64>::min()));
    return value.toInteger();
}
ProviderIdentity readIdentity(const QJsonValue &value)
{
    const auto row = object(value);
    return {providerId(string(row["provider_id"])), string(row["item_id"]), optionalString(row["server_id"])};
}
template<typename Summary>
Summary readTitle(const QJsonObject &row)
{
    Summary result{readIdentity(row["identity"]), string(row["title"])};
    result.year = integer(row["year"]);
    result.posterUrl = optionalString(artwork(optionalString(row["poster_url"])));
    const auto ids = object(row["external_ids"]);
    result.externalIds = {optionalString(ids["imdb"]), optionalString(ids["tmdb"]), optionalString(ids["tvdb"]),
                          optionalString(ids["zap2it"]), optionalString(ids["tvmaze"])};
    return result;
}
ShowSummary readShow(const QJsonValue &value)
{
    const auto row = object(value);
    auto result = readTitle<ShowSummary>(row);
    result.logoUrl = optionalString(artwork(optionalString(row["logo_url"])));
    result.seasonCount = integer(row["season_count"]);
    result.episodeCount = integer(row["episode_count"]);
    return result;
}
MovieSummary readMovie(const QJsonValue &value, bool detail = false)
{
    const auto row = object(value);
    auto result = readTitle<MovieSummary>(row);
    if (detail || row.contains("logo_url"))
        result.logoUrl = optionalString(artwork(optionalString(row["logo_url"])));
    result.durationMs = integer(row["duration_ms"]);
    require(!result.durationMs || *result.durationMs >= 0);
    return result;
}
}

QJsonValue encodeSeries(const QList<ShowSummary> &shows)
{
    QJsonArray result; for (const auto &value : shows) result.append(show(value)); return result;
}
QJsonValue encodeDetail(const ShowDetail &detail)
{
    QJsonArray seasons;
    for (const auto &season : detail.seasons) {
        QJsonArray episodes;
        for (const auto &episode : season.episodes) {
            QJsonArray markers;
            for (const auto &marker : episode.importedSegments) {
                require(std::isfinite(marker.startSeconds) && std::isfinite(marker.endSeconds));
                markers.append(QJsonObject{{"segment_type", segmentName(marker.type)},
                    {"start_seconds", marker.startSeconds}, {"end_seconds", marker.endSeconds}});
            }
            QJsonObject record{{"identity", identity(episode.identity)}, {"title", json(episode.title)},
                {"season_number", json(episode.seasonNumber)}, {"episode_number", json(episode.episodeNumber)},
                {"duration_ms", json(episode.durationMs)}, {"imported_segments", markers}};
            if (episode.numberingSource) record["numbering_source"] = *episode.numberingSource;
            if (episode.episodeOrder) record["episode_order"] = *episode.episodeOrder;
            if (episode.tvdbEpisodeId) record["tvdb_episode_id"] = *episode.tvdbEpisodeId;
            episodes.append(record);
        }
        seasons.append(QJsonObject{{"number", json(season.number)}, {"episodes", episodes}});
    }
    return QJsonObject{{"show", show(detail.show)}, {"seasons", seasons}, {"episode_addressing_version",1}};
}
QList<ShowSummary> decodeSeries(const QJsonValue &value)
{
    QList<ShowSummary> result; for (const auto &row : array(value)) result.append(readShow(row)); return result;
}
ShowDetail decodeDetail(const QJsonValue &value)
{
    const auto root = object(value);
    ShowDetail result{readShow(root["show"]), {}};
    result.hasEpisodeAddressing=root["episode_addressing_version"].toInt()==1;
    for (const auto &value : array(root["seasons"])) {
        const auto row = object(value);
        Season season{integer(row["number"]), {}};
        for (const auto &value : array(row["episodes"])) {
            const auto row = object(value);
            Episode episode{readIdentity(row["identity"])};
            episode.title = optionalString(row["title"]); episode.seasonNumber = integer(row["season_number"]);
            episode.episodeNumber = integer(row["episode_number"]); episode.durationMs = integer(row["duration_ms"]);
            if (row.contains("numbering_source")) episode.numberingSource = optionalString(row["numbering_source"]);
            if (row.contains("episode_order")) episode.episodeOrder = optionalString(row["episode_order"]);
            if (row.contains("tvdb_episode_id")) episode.tvdbEpisodeId = optionalString(row["tvdb_episode_id"]);
            for (const auto &value : array(row["imported_segments"])) {
                const auto marker = object(value);
                require(marker["start_seconds"].isDouble() && marker["end_seconds"].isDouble()
                    && std::isfinite(marker["start_seconds"].toDouble()) && std::isfinite(marker["end_seconds"].toDouble()));
                episode.importedSegments.append({segmentType(string(marker["segment_type"])),
                    marker["start_seconds"].toDouble(), marker["end_seconds"].toDouble()});
            }
            season.episodes.append(episode);
        }
        result.seasons.append(season);
    }
    return result;
}

QJsonValue encodeMovies(const QList<MovieSummary> &movies)
{
    QJsonArray rows;
    for (const auto &value : movies) rows.append(movie(value));
    return rows;
}

QJsonValue encodeMovie(const MovieDetail &detail)
{
    QJsonArray chapters;
    for (const auto &chapter : detail.chapters)
        chapters.append(QJsonObject{{"title", chapter.title}, {"start_ms", chapter.startMs}, {"end_ms", json(chapter.endMs)}});
    return QJsonObject{{"movie", movie(detail.movie)}, {"chapters", chapters}};
}

QList<MovieSummary> decodeMovies(const QJsonValue &value)
{
    QList<MovieSummary> movies;
    for (const auto &row : array(value)) movies.append(readMovie(row));
    return movies;
}

MovieDetail decodeMovie(const QJsonValue &value)
{
    const auto root = object(value);
    MovieDetail result{readMovie(root["movie"], true), {}};
    for (const auto &value : array(root["chapters"])) {
        const auto row = object(value);
        const auto start = integer(row["start_ms"]);
        require(start.has_value());
        result.chapters.append({string(row["title"]), *start, integer(row["end_ms"])});
    }
    normalizeChapters(result);
    return result;
}

QByteArray asciiJson(const QJsonValue &value)
{
    if (value.isArray()) {
        QByteArrayList parts; for (const auto &item : value.toArray()) parts.append(asciiJson(item));
        return '[' + parts.join(", ") + ']';
    }
    if (value.isObject()) {
        QByteArrayList parts; const auto rows = value.toObject();
        for (auto row = rows.begin(); row != rows.end(); ++row) parts.append(asciiJson(row.key()) + ": " + asciiJson(row.value()));
        return '{' + parts.join(", ") + '}';
    }
    const auto encoded = value.toJson(QJsonValue::JsonFormat::Compact);
    QByteArray result;
    for (const auto c : QString::fromUtf8(encoded)) {
        if (c.unicode() > 126) result.append("\\u" + QByteArray::number(c.unicode(), 16).rightJustified(4, '0'));
        else result.append(char(c.unicode()));
    }
    return result;
}

} // namespace introvert::library
