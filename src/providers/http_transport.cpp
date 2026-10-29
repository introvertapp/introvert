#include "http_transport.h"
#include "access_retry.h"

#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslError>
#include <QTimer>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace introvert::providers::detail {

static HttpExchange exchangeOnce(const QUrl &url, const QMap<QByteArray, QByteArray> &headers,
                      double timeoutSeconds, qsizetype maximumBytes, const Cancellation &cancelled,
                      const std::optional<QByteArray> &payload, bool allowHttpErrors, bool headersOnly, bool head)
{
    checkCancelled(cancelled);
    if (!url.isValid() || !std::isfinite(timeoutSeconds) || timeoutSeconds <= 0) throw HttpFailure(HttpFailureKind::Network);
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    for (auto it = headers.cbegin(); it != headers.cend(); ++it) {
        if (it.key().contains('\r') || it.key().contains('\n') || it.value().contains('\r') || it.value().contains('\n'))
            throw HttpFailure(HttpFailureKind::Network);
        request.setRawHeader(it.key(), it.value());
    }
    request.setTransferTimeout(std::chrono::milliseconds(static_cast<qint64>(
        std::clamp(timeoutSeconds * 1000, 1.0, double(std::numeric_limits<int>::max())))));
    QNetworkAccessManager manager;
    std::unique_ptr<QNetworkReply> reply(head ? manager.head(request) : payload ? manager.post(request, *payload) : manager.get(request));
    reply->setReadBufferSize(64 * 1024);
    QEventLoop loop;
    QByteArray data;
    bool exceeded = false;
    bool certificateFailure = false;
    QObject::connect(reply.get(), &QNetworkReply::sslErrors, &loop, [&](const QList<QSslError> &) {certificateFailure=true;});
    auto consume = [&] {
        if (headersOnly) return;
        if (exceeded || !reply->isOpen() || reply->bytesAvailable() <= 0) return;
        data.append(reply->read(std::min<qint64>(reply->bytesAvailable(), maximumBytes + 1 - data.size())));
        if (data.size() > maximumBytes) { exceeded = true; reply->abort(); }
    };
    QObject::connect(reply.get(), &QIODevice::readyRead, &loop, consume);
    QObject::connect(reply.get(), &QNetworkReply::metaDataChanged, &loop, [&] {
        // Webhooks, like the reference sender, need only the response status.
        // Do not download or wait for an unused response body.
        if (headersOnly) {
            if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).isValid()) loop.quit();
            return;
        }
        const auto size = reply->header(QNetworkRequest::ContentLengthHeader);
        if (size.isValid() && size.toLongLong() > maximumBytes) { exceeded = true; reply->abort(); }
    });
    QObject::connect(reply.get(), &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer cancelTimer;
    if (cancelled) {
        cancelTimer.setInterval(10);
        QObject::connect(&cancelTimer, &QTimer::timeout, &loop, [&] {
            if (cancelled->load(std::memory_order_relaxed)) reply->abort();
        });
        cancelTimer.start();
    }
    if (!reply->isFinished()) loop.exec();
    consume(); checkCancelled(cancelled);
    if (exceeded) throw HttpFailure(HttpFailureKind::TooLarge);
    const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    if (!allowHttpErrors && status.isValid() && status.toInt() >= 300) throw HttpFailure(HttpFailureKind::HttpStatus, status.toInt());
    if (headersOnly && status.isValid()) {
        const auto mime = reply->header(QNetworkRequest::ContentTypeHeader).toByteArray();
        QMap<QByteArray, QByteArray> responseHeaders;
        if (head) for (const auto &name : reply->rawHeaderList()) responseHeaders[name.toLower()] = reply->rawHeader(name);
        reply->abort();
        return {{}, mime, status.toInt(), responseHeaders};
    }
    if (reply->error() == QNetworkReply::TimeoutError) throw HttpFailure(HttpFailureKind::Timeout);
    if (!status.isValid() || (reply->error() != QNetworkReply::NoError
        && (!allowHttpErrors || status.toInt() < 300 || int(reply->error()) < 200))) {
        const auto error=reply->error();
        const bool retryable=error==QNetworkReply::ConnectionRefusedError || error==QNetworkReply::RemoteHostClosedError
            || error==QNetworkReply::HostNotFoundError || error==QNetworkReply::TemporaryNetworkFailureError
            || error==QNetworkReply::NetworkSessionFailedError || error==QNetworkReply::UnknownNetworkError
            || error==QNetworkReply::ContentAccessDenied || (error==QNetworkReply::SslHandshakeFailedError && !certificateFailure);
        throw HttpFailure(HttpFailureKind::Network,0,retryable);
    }
    QMap<QByteArray, QByteArray> responseHeaders;
    for (const auto &name : reply->rawHeaderList()) responseHeaders[name.toLower()] = reply->rawHeader(name);
    return {data, reply->header(QNetworkRequest::ContentTypeHeader).toByteArray().split(';').first().trimmed().toLower(),
            status.toInt(), responseHeaders};
}

HttpExchange exchange(const QUrl &url, const QMap<QByteArray, QByteArray> &headers,
                      double timeoutSeconds, qsizetype maximumBytes, const Cancellation &cancelled,
                      const std::optional<QByteArray> &payload, bool allowHttpErrors, bool headersOnly, bool head)
{
    AccessRetry access(url, AccessRetry::defaultEnabled() && !payload.has_value());
    for (;;) {
        try {
            const auto timeout=access.pending() ? std::min(timeoutSeconds,std::max(.001,access.remainingMs()/1000.)) : timeoutSeconds;
            const auto result=exchangeOnce(url,headers,timeout,maximumBytes,cancelled,payload,allowHttpErrors,headersOnly,head);
            access.complete();return result;
        } catch(const HttpFailure &failure) {
            if((!failure.retryable && failure.kind!=HttpFailureKind::Timeout) || !access.pending()) {
                access.complete();throw;
            }
            access.wait(cancelled);
            if(!access.pending()){access.complete();throw;}
        }
    }
}

HttpResponse get(const QUrl &url, const QMap<QByteArray, QByteArray> &headers,
                 double timeoutSeconds, qsizetype maximumBytes, const Cancellation &cancelled)
{
    const auto response = exchange(url, headers, timeoutSeconds, maximumBytes, cancelled);
    return {response.data, response.mime};
}

} // namespace introvert::providers::detail
