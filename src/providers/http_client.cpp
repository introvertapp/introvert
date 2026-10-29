#include "http_client.h"
#include "http_transport.h"

#include <QJsonParseError>
#include <QUrl>
#include <cmath>

namespace introvert::providers {
namespace {

constexpr qsizetype maximumResponseBytes = 16 * 1024 * 1024;

QByteArray quotePlus(const QString &value)
{
    return QUrl::toPercentEncoding(value).replace("%20", "+");
}

QString strippedBase(QString base)
{
    while (base.endsWith('/')) base.chop(1);
    return base;
}

} // namespace

QByteArray HttpClient::read(const ProviderConfig &config, const QString &path, const QVariantMap &parameters,
                            const QString &label, const QMap<QByteArray, QByteArray> &headers,
                            bool image, const Cancellation &cancelled)
{
    checkCancelled(cancelled);
    const auto baseText = config.remoteUrl.value_or(QString());
    const QUrl base(baseText, QUrl::StrictMode);
    if (!base.isValid() || (base.scheme() != "http" && base.scheme() != "https") || base.host().isEmpty()
        || base.authority().contains('@') || !base.query().isEmpty() || !base.fragment().isEmpty())
        throw ProviderError("Enter a valid " + label + " HTTP/HTTPS server URL in Settings → Media.");
    if (!config.credential || config.credential->isEmpty())
        throw ProviderError("Enter your " + label + " API key/token in Settings → Media and save it.");
    const QUrl resource(path, QUrl::StrictMode);
    if (!path.startsWith('/') || path.startsWith("//") || !resource.scheme().isEmpty()
        || !resource.authority().isEmpty() || !resource.query().isEmpty() || !resource.fragment().isEmpty()
        || path.contains('\\') || !resource.isValid())
        throw ProviderError(label + " returned an unsupported resource path.");
    for (auto it = headers.cbegin(); it != headers.cend(); ++it) {
        if (it.key().contains('\r') || it.key().contains('\n') || it.value().contains('\r') || it.value().contains('\n'))
            throw ProviderError("Cannot read " + label + ". Check the URL, network, and HTTPS certificate.");
    }
    if (!std::isfinite(config.timeoutSeconds) || config.timeoutSeconds <= 0)
        throw ProviderError(label + " request timeout must be a finite positive number of seconds.");
    auto encodedUrl = strippedBase(baseText).toUtf8() + path.toUtf8();
    QByteArrayList query;
    for (auto it = parameters.cbegin(); it != parameters.cend(); ++it) {
        const auto value = it.value().metaType().id() == QMetaType::Bool
            ? (it.value().toBool() ? QStringLiteral("true") : QStringLiteral("false")) : it.value().toString();
        query.append(quotePlus(it.key()) + '=' + quotePlus(value));
    }
    if (!query.isEmpty()) encodedUrl += '?' + query.join('&');
    const auto url = QUrl::fromEncoded(encodedUrl, QUrl::StrictMode);
    if (!url.isValid()) throw ProviderError(label + " returned an unsupported resource path.");
    auto outgoing = headers;
    outgoing["Accept"] = image ? "image/*" : "application/json";
    detail::HttpResponse response;
    try { response = detail::get(url, outgoing, config.timeoutSeconds, maximumResponseBytes, cancelled); }
    catch (const detail::HttpFailure &error) {
        if (error.kind == detail::HttpFailureKind::TooLarge) throw ProviderError(label + " response exceeded the supported size.");
        if (error.kind == detail::HttpFailureKind::Timeout) throw ProviderError(label + " request timed out. Check the server and retry.");
        if (error.kind == detail::HttpFailureKind::HttpStatus) {
            const auto status = error.status;
            QString message;
            if (status == 401 || status == 403) message = label + " rejected the API key/token. Check Media settings and server access.";
            else if (status == 404) message = "The requested " + label + " resource was not found.";
            else if (status < 400) message = label + " redirected the request. Enter the final server URL in Media settings.";
            else message = label + " could not complete the request. Check the server and retry.";
            throw ProviderError(message, status);
        }
        throw ProviderError("Cannot read " + label + ". Check the URL, network, and HTTPS certificate.");
    }
    if (image && response.mime != "image/png" && response.mime != "image/jpeg" && response.mime != "image/webp")
        throw ProviderError(label + " returned unsupported artwork.");
    return response.data;
}

QJsonValue HttpClient::json(const ProviderConfig &config, const QString &path, const QVariantMap &parameters,
                            const QString &label, const QMap<QByteArray, QByteArray> &headers, const Cancellation &cancelled)
{
    const auto data = read(config, path, parameters, label, headers, false, cancelled);
    QJsonParseError error;
    const auto result = QJsonValue::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError)
        throw ProviderError(label + " returned an invalid media response.");
    return result;
}

QByteArray HttpClient::image(const ProviderConfig &config, const QString &path, const QString &label,
                             const QMap<QByteArray, QByteArray> &headers, const Cancellation &cancelled)
{
    return read(config, path, {}, label, headers, true, cancelled);
}

} // namespace introvert::providers
