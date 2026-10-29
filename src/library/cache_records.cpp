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
QJsonObject show(const ShowSummary &value)
{
    return {{"identity", identity(value.identity)}, {"title", value.title}, {"year", json(value.year)},
        {"poster_url", artwork(value.posterUrl)}, {"logo_url", artwork(value.logoUrl)},
        {"external_ids", QJsonObject{{"imdb", json(value.externalIds.imdb)}, {"tmdb", json(value.externalIds.tmdb)},
            {"tvdb", json(value.externalIds.tvdb)}, {"zap2it", json(value.externalIds.zap2it)}, {"tvmaze", json(value.externalIds.tvmaze)}}},
        {"season_count", json(value.seasonCount)}, {"episode_count", json(value.episodeCount)}};
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
ShowSummary readShow(const QJsonValue &value)
{
    const auto row = object(value);
    ShowSummary result{readIdentity(row["identity"]), string(row["title"])};
    result.year = integer(row["year"]); result.seasonCount = integer(row["season_count"]); result.episodeCount = integer(row["episode_count"]);
    result.posterUrl = optionalString(artwork(optionalString(row["poster_url"])));
    result.logoUrl = optionalString(artwork(optionalString(row["logo_url"])));
    const auto ids = object(row["external_ids"]);
    result.externalIds = {optionalString(ids["imdb"]), optionalString(ids["tmdb"]), optionalString(ids["tvdb"]),
                          optionalString(ids["zap2it"]), optionalString(ids["tvmaze"])};
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
            episodes.append(QJsonObject{{"identity", identity(episode.identity)}, {"title", json(episode.title)},
                {"season_number", json(episode.seasonNumber)}, {"episode_number", json(episode.episodeNumber)},
                {"duration_ms", json(episode.durationMs)}, {"imported_segments", markers}});
        }
        seasons.append(QJsonObject{{"number", json(season.number)}, {"episodes", episodes}});
    }
    return QJsonObject{{"show", show(detail.show)}, {"seasons", seasons}};
}
QList<ShowSummary> decodeSeries(const QJsonValue &value)
{
    QList<ShowSummary> result; for (const auto &row : array(value)) result.append(readShow(row)); return result;
}
ShowDetail decodeDetail(const QJsonValue &value)
{
    const auto root = object(value);
    ShowDetail result{readShow(root["show"]), {}};
    for (const auto &value : array(root["seasons"])) {
        const auto row = object(value);
        Season season{integer(row["number"]), {}};
        for (const auto &value : array(row["episodes"])) {
            const auto row = object(value);
            Episode episode{readIdentity(row["identity"])};
            episode.title = optionalString(row["title"]); episode.seasonNumber = integer(row["season_number"]);
            episode.episodeNumber = integer(row["episode_number"]); episode.durationMs = integer(row["duration_ms"]);
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
