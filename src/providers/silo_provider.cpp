#include "silo_provider.h"
#include "http_client.h"
#include "json_values.h"
#include "metadata_client.h"

#include <QBuffer>
#include <QDebug>
#include <QDateTime>
#include <QImage>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <limits>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

namespace introvert::providers {
namespace {

const QString catalogItems = QStringLiteral("/api/v2/catalog/items/");
// Stable adapter references, not delivery URLs. Catalog cache records never
// retain Silo's short-lived signed artwork URLs. artwork() resolves these fields
// afresh through the official catalog API when an image needs downloading.
const QString artworkPrefix = QStringLiteral("/silo-artwork/");

qint64 retryDelay(const QByteArray &header, int attempt)
{
    const auto value = header.trimmed();
    bool secondsValid = false;
    const auto seconds = value.toLongLong(&secondsValid);
    if (secondsValid && seconds >= 0)
        return seconds > 120 ? 120001 : std::max<qint64>(100, seconds * 1000);
    const auto date = QDateTime::fromString(QString::fromLatin1(value), Qt::RFC2822Date);
    if (date.isValid()) return std::max<qint64>(100, QDateTime::currentDateTimeUtc().msecsTo(date));
    return 1000LL << attempt;
}

QByteArray displayArtwork(const QByteArray &bytes, const Cancellation &cancel)
{
    checkCancelled(cancel);
    if (bytes.size() < 12 || !bytes.startsWith("RIFF") || bytes.mid(8, 4) != "WEBP") return bytes;
    // The approved Qt kits do not all include an imageformats WebP plugin.
    // Decode downloaded bytes with the FFmpeg runtime already shipped for
    // playback, preserving alpha, then cache a PNG Qt can always display.
    const auto codec = avcodec_find_decoder(AV_CODEC_ID_WEBP);
    if (!codec) throw ProviderError("The bundled media runtime cannot decode Silo WebP artwork.");
    const auto freeContext = [](AVCodecContext *value) { avcodec_free_context(&value); };
    const auto freePacket = [](AVPacket *value) { av_packet_free(&value); };
    const auto freeFrame = [](AVFrame *value) { av_frame_free(&value); };
    std::unique_ptr<AVCodecContext, decltype(freeContext)> context(avcodec_alloc_context3(codec), freeContext);
    std::unique_ptr<AVPacket, decltype(freePacket)> packet(av_packet_alloc(), freePacket);
    std::unique_ptr<AVFrame, decltype(freeFrame)> frame(av_frame_alloc(), freeFrame);
    constexpr qint64 maxPixels = 16 * 1024 * 1024;
    if (!context || !packet || !frame || bytes.size() > 20 * 1024 * 1024)
        throw ProviderError("Silo artwork exceeded the supported image budget.");
    context->max_pixels = maxPixels;
    context->thread_count = 1;
    if (avcodec_open2(context.get(), codec, nullptr) < 0 || av_new_packet(packet.get(), int(bytes.size())) < 0)
        throw ProviderError("Cannot initialize the Silo artwork decoder.");
    std::memcpy(packet->data, bytes.constData(), size_t(bytes.size()));
    if (avcodec_send_packet(context.get(), packet.get()) < 0 || avcodec_receive_frame(context.get(), frame.get()) < 0)
        throw ProviderError("Silo returned invalid WebP artwork.");
    checkCancelled(cancel);
    if (frame->width <= 0 || frame->height <= 0 || qint64(frame->width) * frame->height > maxPixels)
        throw ProviderError("Silo artwork exceeded the supported image dimensions.");
    QImage decoded(frame->width, frame->height, QImage::Format_RGBA8888);
    if (decoded.isNull()) throw ProviderError("Cannot allocate the Silo artwork image.");
    std::unique_ptr<SwsContext, decltype(&sws_freeContext)> scaler(sws_getContext(frame->width, frame->height,
        AVPixelFormat(frame->format), frame->width, frame->height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr), sws_freeContext);
    uint8_t *planes[] = {decoded.bits(), nullptr, nullptr, nullptr};
    const int strides[] = {int(decoded.bytesPerLine()), 0, 0, 0};
    if (!scaler || sws_scale(scaler.get(), frame->data, frame->linesize, 0, frame->height, planes, strides) != frame->height)
        throw ProviderError("Cannot convert Silo artwork for display.");
    QByteArray png;
    QBuffer output(&png);
    if (!output.open(QIODevice::WriteOnly) || !decoded.save(&output, "PNG") || png.size() > 20 * 1024 * 1024)
        throw ProviderError("Cannot encode Silo artwork for the image cache.");
    checkCancelled(cancel);
    return png;
}

QString identifier(const QJsonValue &value)
{
    if (!value.isString() || value.toString().isEmpty())
        throw ProviderError("Silo returned an invalid media identifier.");
    return value.toString();
}

QString fileId(const QJsonValue &value)
{
    const auto id = identifier(value);
    static const QRegularExpression decimal(QStringLiteral("\\A[1-9][0-9]*\\z"));
    if (!decimal.match(id).hasMatch()) throw ProviderError("Silo returned an invalid media file identifier.");
    return id;
}

std::optional<qint64> nonnegativeInteger(const QJsonValue &value)
{
    if (!value.isDouble() || std::trunc(value.toDouble()) != value.toDouble()) return {};
    return detail::nonnegativeNumber(value);
}

std::optional<qint64> milliseconds(const QJsonValue &value, double scale = 1000)
{
    if (!value.isDouble()) return {};
    const auto number = value.toDouble() * scale;
    if (!std::isfinite(number) || number < 0 || number >= double(std::numeric_limits<qint64>::max())) return {};
    return qint64(number);
}

QJsonObject version(const QJsonObject &item)
{
    if (!item["versions"].isArray()) throw ProviderError("Silo returned an invalid media version list.");
    for (const auto &value : item["versions"].toArray()) {
        if (!value.isObject()) throw ProviderError("Silo returned an invalid media version.");
        const auto source = value.toObject();
        if (source["unreadable"].toBool()) continue;
        fileId(source["file_id"]);
        return source;
    }
    return {};
}

QList<ImportedSegment> markers(const QJsonValue &value, const QString &file, std::optional<qint64> duration)
{
    if (!value.isObject()) throw ProviderError("Silo returned an invalid marker response.");
    const auto object = value.toObject();
    if (fileId(object["file_id"]) != file || !object["marker_segments"].isArray())
        throw ProviderError("Silo returned an invalid marker response.");
    QList<ImportedSegment> result;
    QSet<QString> seen;
    for (const auto &entry : object["marker_segments"].toArray()) {
        const auto row = entry.toObject();
        const auto kind = row["kind"].toString();
        if (!QStringList{"intro", "credits", "recap", "preview"}.contains(kind) || seen.contains(kind)) continue;
        if (!row["start_seconds"].isDouble() || !row["end_seconds"].isDouble()) continue;
        const auto start = row["start_seconds"].toDouble(), end = row["end_seconds"].toDouble();
        if (!std::isfinite(start) || !std::isfinite(end) || start < 0 || end <= start
            || (duration && *duration > 0 && end > *duration / 1000.)) continue;
        result.append({segmentType(kind), start, end});
        seen.insert(kind);
    }
    return result;
}

} // namespace

void SiloProvider::invalidate()
{
    m_profileAge.invalidate(); m_url.reset(); m_credential.reset(); m_profile.clear();
}

QMap<QByteArray, QByteArray> SiloProvider::accessHeaders(const ProviderConfig &config, const Cancellation &cancel)
{
    checkCancelled(cancel);
    if (config.provider != ProviderId::Silo) throw ProviderError("The saved media provider does not match this request.");
    QMap<QByteArray, QByteArray> headers{{"Authorization", "Bearer " + config.credential.value_or(QString()).toUtf8()}};
    if (!m_profileAge.isValid() || m_profileAge.elapsed() >= 300000 || m_url != config.remoteUrl || m_credential != config.credential) {
        // Integration keys belong to an account and may select its primary
        // profile without PIN verification. No interactive sign-in is needed.
        const auto value = HttpClient::json(config, "/api/v2/profiles", {}, "Silo", headers, cancel);
        if (!value.isObject() || !value.toObject()["items"].isArray())
            throw ProviderError("Silo returned an invalid profile list.");
        QString selected;
        const auto profiles = value.toObject()["items"].toArray();
        for (const auto &profile : profiles) {
            if (!profile.isObject()) throw ProviderError("Silo returned an invalid profile list.");
            const auto row = profile.toObject();
            const auto id = identifier(row["id"]);
            if (row["is_primary"].toBool()) { selected = id; break; }
        }
        if (selected.isEmpty() && profiles.size() == 1) selected = identifier(profiles.first().toObject()["id"]);
        if (selected.isEmpty()) throw ProviderError("No primary Silo profile is available for media access.");
        m_url = config.remoteUrl; m_credential = config.credential; m_profile = selected; m_profileAge.start();
    }
    headers["X-Profile-Id"] = m_profile.toUtf8();
    return headers;
}

QJsonValue SiloProvider::request(const ProviderConfig &config, const QString &path, const QVariantMap &parameters,
                                const Cancellation &cancel)
{
    // Never route this provider through compatibility or legacy API endpoints.
    if (!path.startsWith("/api/v2/")) throw ProviderError("Silo returned an unsupported resource path.");
    qint64 waited = 0;
    for (int attempt = 0;; ++attempt) {
        try { return HttpClient::json(config, path, parameters, "Silo", accessHeaders(config, cancel), cancel); }
        catch (const ProviderError &error) {
            if (error.httpStatus() == 429) {
                const auto delay = retryDelay(error.retryAfter(), attempt);
                // Read-only native calls can resume after a short server cooldown.
                // Bound attempts and total waiting; cancellation/shutdown stay prompt.
                if (attempt < 3 && delay <= 120000 - waited) {
                    qInfo().noquote() << QStringLiteral("Silo rate limit reached; retrying in %1 seconds.").arg(delay / 1000.);
                    QElapsedTimer clock; clock.start();
                    while (clock.elapsed() < delay) { checkCancelled(cancel); QThread::msleep(25); }
                    checkCancelled(cancel); waited += delay;
                    continue;
                }
            }
            if (!error.httpStatus()) throw;
            // Include only the status and fixed API operation, never a server URL,
            // response body, identifier, signed URL or credential.
            const auto operation = path.endsWith("/seasons") ? QStringLiteral("season list") : QStringLiteral("catalog");
            throw ProviderError(QStringLiteral("%1 (HTTP %2; native %3 API)")
                .arg(QString::fromUtf8(error.what())).arg(*error.httpStatus()).arg(operation), error.httpStatus(), error.retryAfter());
        }
    }
}

QList<QJsonObject> SiloProvider::items(const ProviderConfig &config, const QString &path,
                                     const QVariantMap &parameters, const Cancellation &cancel)
{
    QList<QJsonObject> result;
    auto query = parameters;
    QSet<QString> cursors;
    while (true) {
        const auto value = request(config, path, query, cancel);
        if (!value.isObject() || !value.toObject()["items"].isArray())
            throw ProviderError("Silo returned an invalid item list.");
        const auto object = value.toObject();
        for (const auto &entry : object["items"].toArray()) {
            if (!entry.isObject()) throw ProviderError("Silo returned an invalid media response.");
            result.append(entry.toObject());
        }
        if (!object.contains("page")) return result;
        if (!object["page"].isObject() || !object["page"].toObject()["has_more"].isBool())
            throw ProviderError("Silo returned invalid pagination.");
        const auto page = object["page"].toObject();
        if (!page["has_more"].toBool()) return result;
        const auto cursor = identifier(page["next_cursor"]);
        if (cursors.contains(cursor)) throw ProviderError("Silo returned repeated pagination.");
        cursors.insert(cursor); query["cursor"] = cursor;
    }
}

QJsonObject SiloProvider::item(const ProviderConfig &config, const QString &id, const Cancellation &cancel)
{
    const auto value = request(config, catalogItems + detail::quote(id), {}, cancel);
    if (!value.isObject() || identifier(value.toObject()["content_id"]) != id)
        throw ProviderError("Silo returned an invalid media response.");
    return value.toObject();
}

template<typename Summary>
Summary SiloProvider::title(const QJsonObject &row, bool images) const
{
    const auto id = identifier(row["content_id"]);
    Summary result{{ProviderId::Silo, id, {}}, row["title"].toString()};
    result.year = nonnegativeInteger(row["year"]);
    const auto external = [&](const char *key) -> std::optional<QString> {
        const auto value = row[key].toString();
        return value.isEmpty() ? std::nullopt : std::optional<QString>(value);
    };
    result.externalIds = {external("imdb_id"), external("tmdb_id"), external("tvdb_id"), {}, {}};
    if (images) {
        if (!row["poster_url"].toString().isEmpty()) result.posterUrl = artworkPrefix + "poster/" + detail::quote(id);
        if (!row["logo_url"].toString().isEmpty()) result.logoUrl = artworkPrefix + "logo/" + detail::quote(id);
    }
    return result;
}

ShowSummary SiloProvider::show(const ProviderConfig &config, const QJsonObject &row, bool images) const
{
    auto result = title<ShowSummary>(row, images);
    result.seasonCount = nonnegativeInteger(row["season_count"]);
    result.episodeCount = nonnegativeInteger(row["episode_count"]);
    result.itemUrl = itemWebUrl(config.remoteUrl.value_or(QString()), result.identity);
    return result;
}

QList<ShowSummary> SiloProvider::listSeries(const ProviderConfig &config, bool images, const Cancellation &cancel)
{
    QList<ShowSummary> result;
    QSet<QString> seen;
    for (const auto &row : items(config, "/api/v2/catalog", {{"source", "query"}, {"type", "series"}, {"limit", 200}}, cancel)) {
        if (row["type"] != "series") continue;
        auto summary = show(config, row, images);
        if (seen.contains(summary.identity.itemId)) continue;
        seen.insert(summary.identity.itemId);
        if (!summary.seasonCount || !summary.episodeCount) {
            // Native catalog cards omit these rollups. Populate the inventory
            // used by poster hover details and the library episode total from
            // the lightweight season list, without fetching every episode.
            try {
                qint64 episodes = 0;
                QSet<qint64> seasons;
                const auto path = QStringLiteral("/api/v2/catalog/series/") + detail::quote(summary.identity.itemId) + "/seasons";
                for (const auto &season : items(config, path, {{"include_artwork", false}}, cancel)) {
                    const auto number = nonnegativeInteger(season["season_number"]);
                    const auto count = nonnegativeInteger(season["episode_count"]);
                    if (!number || !count)
                        throw ProviderError("Silo returned invalid season episode counts.");
                    if (seasons.contains(*number)) continue;
                    if (*count > std::numeric_limits<qint64>::max() - episodes)
                        throw ProviderError("Silo returned invalid season episode counts.");
                    seasons.insert(*number); episodes += *count;
                }
                summary.seasonCount = seasons.size(); summary.episodeCount = episodes;
            } catch (const ProviderError &error) {
                // Optional rollups for one unavailable title must not prevent
                // the rest of the catalog from loading. Authentication, rate
                // limits and invalid responses still fail the refresh normally.
                const auto status = error.httpStatus().value_or(0);
                if (status != 404 && (status < 500 || status > 599)) throw;
                if (status != 404)
                    qWarning().noquote() << QStringLiteral("Silo season counts unavailable for %1 (HTTP %2). Refresh to retry.")
                        .arg(summary.title).arg(status);
            }
        }
        result.append(std::move(summary));
    }
    std::stable_sort(result.begin(), result.end(), [](const auto &a, const auto &b) { return a.title.toCaseFolded() < b.title.toCaseFolded(); });
    return result;
}

QList<MovieSummary> SiloProvider::listMovies(const ProviderConfig &config, bool images, const Cancellation &cancel)
{
    QList<MovieSummary> result;
    QSet<QString> seen;
    for (const auto &row : items(config, "/api/v2/catalog", {{"source", "query"}, {"type", "movie"}, {"limit", 200}}, cancel)) {
        if (row["type"] != "movie") continue;
        auto summary = title<MovieSummary>(row, images);
        summary.durationMs = milliseconds(row["duration_seconds"]);
        if (!seen.contains(summary.identity.itemId)) { seen.insert(summary.identity.itemId); result.append(std::move(summary)); }
    }
    std::stable_sort(result.begin(), result.end(), [](const auto &a, const auto &b) { return a.title.toCaseFolded() < b.title.toCaseFolded(); });
    return result;
}

std::optional<ShowDetail> SiloProvider::getShow(const ProviderConfig &config, const QString &id, bool images, const Cancellation &cancel)
{
    QJsonObject metadata;
    try { metadata = item(config, id, cancel); }
    catch (const ProviderError &error) { if (error.httpStatus() == 404) return {}; throw; }
    if (metadata["type"] != "series") return {};
    ShowDetail result{show(config, metadata, images), {}, true};
    QMap<qint64, QList<Episode>> seasons;
    QSet<QString> seen;
    const auto series = QStringLiteral("/api/v2/catalog/series/") + detail::quote(id) + "/seasons";
    for (const auto &season : items(config, series, {{"include_artwork", false}}, cancel)) {
        const auto number = nonnegativeInteger(season["season_number"]);
        if (!number) throw ProviderError("Silo returned an invalid season number.");
        if (seasons.contains(*number)) continue;
        seasons.insert(*number, {});
        // /catalog/items/{id}/episodes is for a season item, never a series.
        for (const auto &row : items(config, series + '/' + QString::number(*number) + "/episodes", {}, cancel)) {
            const auto episodeId = identifier(row["content_id"]);
            if (seen.contains(episodeId)) continue;
            seen.insert(episodeId);
            Episode episode{{ProviderId::Silo, episodeId, {}}};
            episode.title = detail::optionalString(row["title"]);
            episode.seasonNumber = number;
            if (row.contains("season_number") && nonnegativeInteger(row["season_number"]) != number)
                throw ProviderError("Silo returned an episode from a different season.");
            episode.episodeNumber = nonnegativeInteger(row["episode_number"]);
            episode.itemUrl = itemWebUrl(config.remoteUrl.value_or(QString()), episode.identity);
            const auto tvdb = row["tvdb_id"].toString();
            if (!tvdb.isEmpty()) { episode.tvdbEpisodeId = tvdb; episode.numberingSource = "tvdb"; }
            // Season listings already carry episode identity and numbering.
            // File/watch detail can trigger expensive server-side marker work;
            // it is needed here only for explicitly enabled timestamp imports.
            if (config.importSegments) {
                const auto detail = item(config, episodeId, cancel);
                if (detail["type"] != "episode") throw ProviderError("Silo returned an invalid episode response.");
                const auto source = version(detail);
                episode.durationMs = milliseconds(source["duration"]);
                if (!episode.durationMs) episode.durationMs = milliseconds(detail["duration_seconds"]);
                if (!source.isEmpty()) {
                    const auto file = fileId(source["file_id"]);
                    episode.importedSegments = markers(source["marker_segments"].isArray()
                        ? QJsonValue(QJsonObject{{"file_id", file}, {"marker_segments", source["marker_segments"]}})
                        : request(config, "/api/v2/markers/files/" + file, {}, cancel), file, episode.durationMs);
                }
            }
            seasons[*number].append(std::move(episode));
        }
    }
    if (!result.show.seasonCount) result.show.seasonCount = seasons.size();
    if (!result.show.episodeCount) result.show.episodeCount = seen.size();
    for (auto it = seasons.begin(); it != seasons.end(); ++it) {
        std::stable_sort(it->begin(), it->end(), [](const auto &a, const auto &b) { return a.episodeNumber < b.episodeNumber; });
        result.seasons.append({it.key(), *it});
    }
    return result;
}

std::optional<MovieDetail> SiloProvider::getMovie(const ProviderConfig &config, const QString &id, bool images, const Cancellation &cancel)
{
    QJsonObject metadata;
    try { metadata = item(config, id, cancel); }
    catch (const ProviderError &error) { if (error.httpStatus() == 404) return {}; throw; }
    if (metadata["type"] != "movie") return {};
    MovieDetail result{title<MovieSummary>(metadata, images), {}};
    const auto source = version(metadata);
    result.movie.durationMs = milliseconds(source["duration"]);
    if (!result.movie.durationMs) result.movie.durationMs = milliseconds(metadata["duration_seconds"]);
    for (const auto &entry : source["chapters"].toArray()) {
        const auto row = entry.toObject();
        const auto start = milliseconds(row["start_seconds"]);
        if (start) result.chapters.append({row["title"].toString(), *start, milliseconds(row["end_seconds"])});
    }
    normalizeChapters(result);
    return result;
}

QString SiloProvider::mediaFile(const ProviderConfig &config, const QString &id, const Cancellation &cancel)
{
    const auto metadata = item(config, id, cancel);
    if (metadata["type"] != "episode" && metadata["type"] != "movie")
        throw ProviderError("Silo did not return a playable media item.");
    const auto source = version(metadata);
    if (source.isEmpty()) throw ProviderError("Silo did not return an accessible media source.");
    if (source["presentation_part_total"].toInt() > 1)
        throw ProviderError("Streaming this Silo title requires a single media file.");
    return fileId(source["file_id"]);
}

QByteArray SiloProvider::artwork(const ProviderConfig &config, const QString &reference, const Cancellation &cancel)
{
    if (!reference.startsWith(artworkPrefix)) throw ProviderError("Silo returned an unsupported artwork reference.");
    const auto suffix = reference.mid(artworkPrefix.size());
    const auto slash = suffix.indexOf('/');
    const auto kind = suffix.left(slash);
    const auto encoded = suffix.mid(slash + 1);
    const auto id = QUrl::fromPercentEncoding(encoded.toUtf8());
    if (slash < 0 || (kind != "poster" && kind != "logo") || id.isEmpty() || detail::quote(id) != encoded)
        throw ProviderError("Silo returned an unsupported artwork reference.");
    const auto metadata = item(config, id, cancel);
    const auto address = metadata[kind + "_url"].toString();
    if (address.isEmpty()) throw ProviderError("Silo artwork is no longer available.", 404);
    const QUrl base(detail::baseUrl(config.remoteUrl.value_or(QString())) + '/');
    const QUrl resource(address, QUrl::StrictMode);
    if (!resource.isValid() || address.startsWith("//") || address.contains('\\') || resource.hasFragment())
        throw ProviderError("Silo returned an unsupported artwork URL.");
    // Silo's server-relative API artwork paths use the same configured proxy
    // prefix as metadata. Already-prefixed and absolute delivery URLs retain
    // their own path.
    const auto url = address.startsWith("/api/v2/")
        ? QUrl(detail::baseUrl(config.remoteUrl.value_or(QString())) + address, QUrl::StrictMode)
        : base.resolved(resource);
    if (base.scheme() == "https" && url.scheme() != "https") throw ProviderError("Silo returned an insecure artwork URL.");
    // Native artwork is public/presigned, including storage/CDN delivery. The
    // shared image downloader follows bounded redirects without any API key.
    return displayArtwork(MetadataClient::image(url.toString(QUrl::FullyEncoded), cancel), cancel);
}

} // namespace introvert::providers
