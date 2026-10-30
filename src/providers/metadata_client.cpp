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
detail::HttpResponse read(const QString &urlText, const QVariantMap &parameters, bool image,
                          double timeoutSeconds, const Cancellation &cancelled)
{
    checkCancelled(cancelled);
    const QUrl url(urlText, QUrl::StrictMode);
    if (!url.isValid() || (url.scheme() != "http" && url.scheme() != "https") || url.host().isEmpty() || url.authority().contains('@'))
        throw ProviderError(QStringLiteral("Unsupported metadata resource URL"));
    auto encoded = urlText.toUtf8();
    QByteArrayList query;
    for (auto it = parameters.begin(); it != parameters.end(); ++it)
        query.append(QUrl::toPercentEncoding(it.key()).replace("%20", "+") + '='
                     + QUrl::toPercentEncoding(it.value().toString()).replace("%20", "+"));
    if (!query.isEmpty()) encoded += '?' + query.join('&');
    try {
        const auto result = detail::get(QUrl::fromEncoded(encoded, QUrl::StrictMode),
            {{"Accept", image ? "image/*" : "application/json"}}, timeoutSeconds, 20 * 1024 * 1024, cancelled);
        if (image && !result.mime.startsWith("image/")) throw ProviderError(QStringLiteral("Unsupported metadata artwork"));
        return result;
    } catch (const detail::HttpFailure &error) {
        if (error.kind == detail::HttpFailureKind::TooLarge) throw ProviderError(QStringLiteral("Metadata response exceeded the supported size"));
        if (error.kind == detail::HttpFailureKind::HttpStatus && error.status == 404) throw MetadataNotFound();
        throw ProviderError(QStringLiteral("Metadata request failed. Check network access, API key and server availability."));
    }
}
}
QJsonValue MetadataClient::read(const QString &url, const QVariantMap &parameters, double timeoutSeconds, const Cancellation &cancelled)
{
    const auto response = providers::read(url, parameters, false, timeoutSeconds, cancelled);
    QJsonParseError error;
    const auto result = QJsonValue::fromJson(response.data, &error);
    if (error.error != QJsonParseError::NoError)
        throw ProviderError(QStringLiteral("Metadata request failed. Check network access, API key and server availability."));
    return result;
}
QByteArray MetadataClient::image(const QString &url, const Cancellation &cancelled)
{
    return providers::read(url, {}, true, 10, cancelled).data;
}

MetadataClient::MetadataClient(QString tvmaze, QString tmdb, double delaySeconds, QVariantMap additionalBases)
    : m_tvmaze(std::move(tvmaze)), m_tmdb(std::move(tmdb)), m_delay(delaySeconds), m_bases(std::move(additionalBases))
{
    m_bases["tvmaze"]=m_tvmaze; m_bases["tmdb"]=m_tmdb;
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
