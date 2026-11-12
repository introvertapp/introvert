#include "metadata_client.h"
#include "http_transport.h"
#include "json_values.h"

#include <QJsonParseError>
#include <QJsonArray>
#include <QJsonObject>
#include <QElapsedTimer>
#include <QThread>

namespace introvert::providers {
namespace {
bool resourceUrl(const QUrl &url)
{
    return url.isValid() && (url.scheme() == "http" || url.scheme() == "https")
        && !url.host().isEmpty() && !url.authority().contains('@');
}

detail::HttpResponse read(const QString &urlText, const QVariantMap &parameters, double timeoutSeconds,
                          const Cancellation &cancelled)
{
    checkCancelled(cancelled);
    if (!resourceUrl(QUrl(urlText, QUrl::StrictMode))) throw ProviderError("Unsupported metadata resource URL");
    auto encoded = urlText.toUtf8();
    QByteArrayList query;
    for (auto it = parameters.begin(); it != parameters.end(); ++it)
        query.append(QUrl::toPercentEncoding(it.key()).replace("%20", "+") + '='
                     + QUrl::toPercentEncoding(it.value().toString()).replace("%20", "+"));
    if (!query.isEmpty()) encoded += '?' + query.join('&');
    try {
        return detail::get(QUrl::fromEncoded(encoded, QUrl::StrictMode),
            {{"Accept", "application/json"}}, timeoutSeconds, 20 * 1024 * 1024, cancelled);
    } catch (const detail::HttpFailure &error) {
        if (error.kind == detail::HttpFailureKind::TooLarge) throw ProviderError(QStringLiteral("Metadata response exceeded the supported size"));
        if (error.kind == detail::HttpFailureKind::HttpStatus && error.status == 404) throw MetadataNotFound();
        throw ProviderError(QStringLiteral("Metadata request failed. Check network access, API key and server availability."));
    }
}
}
QJsonValue MetadataClient::read(const QString &url, const QVariantMap &parameters, double timeoutSeconds, const Cancellation &cancelled)
{
    const auto response = providers::read(url, parameters, timeoutSeconds, cancelled);
    QJsonParseError error;
    const auto result = QJsonValue::fromJson(response.data, &error);
    if (error.error != QJsonParseError::NoError)
        throw ProviderError(QStringLiteral("Metadata request failed. Check network access, API key and server availability."));
    return result;
}
QByteArray MetadataClient::image(const QString &url, const Cancellation &cancelled)
{
    checkCancelled(cancelled);
    auto current = QUrl(url, QUrl::StrictMode);
    for (int redirects = 0; redirects <= 3; ++redirects) {
        if (!resourceUrl(current)) throw ProviderError("Unsupported artwork URL.");
        detail::HttpExchange response;
        try {
            response = detail::exchange(current, {{"Accept", "image/*"}}, 10, 20 * 1024 * 1024, cancelled, {}, true);
        } catch (const detail::HttpFailure &error) {
            if (error.kind == detail::HttpFailureKind::TooLarge) throw ProviderError("Artwork response exceeded the supported size.");
            throw ProviderError("Artwork could not be downloaded. Check network access and retry.");
        }
        if (response.status == 200) {
            if (!response.mime.startsWith("image/") && response.mime != "application/octet-stream")
                throw ProviderError("Artwork server returned an unsupported content type.");
            return response.data;
        }
        const auto location = response.headers.value("location");
        if (response.status < 300 || response.status >= 400 || location.isEmpty())
            throw ProviderError(QString("Artwork download failed (HTTP %1).").arg(response.status), response.status);
        const auto next = current.resolved(QUrl::fromEncoded(location, QUrl::StrictMode));
        if (current.scheme() == "https" && next.scheme() != "https")
            throw ProviderError("Artwork redirected to an insecure URL.");
        current = next;
    }
    throw ProviderError("Artwork download exceeded the redirect limit.");
}

MetadataClient::MetadataClient(QString tvmaze, QString tmdb, double delaySeconds, QVariantMap additionalBases)
    : m_tvmaze(std::move(tvmaze)), m_tmdb(std::move(tmdb)), m_delay(delaySeconds), m_bases(std::move(additionalBases))
{
    m_bases["tvmaze"]=m_tvmaze; m_bases["tmdb"]=m_tmdb;
    if(!m_bases.contains("tmdb_images"))m_bases["tmdb_images"]="https://image.tmdb.org/t/p/w500";
    if(!m_bases.contains("tvdb"))m_bases["tvdb"]="https://api4.thetvdb.com/v4";
    if(!m_bases.contains("omdb"))m_bases["omdb"]="https://www.omdbapi.com";
    if(!m_bases.contains("fanart"))m_bases["fanart"]="https://webservice.fanart.tv/v3.2";
}

QJsonValue MetadataClient::tv(const QString &path, const QVariantMap &parameters, const Cancellation &cancelled) const
{
    QElapsedTimer delay; delay.start();
    while (delay.elapsed() < m_delay * 1000) {
        checkCancelled(cancelled);
        QThread::msleep(10);
    }
    checkCancelled(cancelled);
    return read(m_tvmaze + path, parameters, 10, cancelled);
}

std::optional<qint64> MetadataClient::positiveId(const QJsonValue &value)
{
    const auto number = detail::finiteNumber(value);
    if (!number || std::floor(*number) != *number) return std::nullopt;
    return detail::nonnegativeNumber(value, true);
}

} // namespace introvert::providers
