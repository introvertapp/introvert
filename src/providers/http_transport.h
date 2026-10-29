#pragma once

#include "contracts.h"
#include <QMap>
#include <QUrl>

namespace introvert::providers::detail {

enum class HttpFailureKind { HttpStatus, Timeout, Network, TooLarge };
class HttpFailure final : public std::exception {
public:
    HttpFailure(HttpFailureKind kind, int status = 0, bool retryable = false) : kind(kind), status(status), retryable(retryable) {}
    const char *what() const noexcept override { return "Media request failed"; }
    HttpFailureKind kind;
    int status;
    bool retryable;
};
struct HttpResponse { QByteArray data, mime; };
struct HttpExchange {
    QByteArray data, mime;
    int status;
    QMap<QByteArray, QByteArray> headers;
};

// Endpoint submissions need POST and the complete HTTP status/reset headers.
// The default retains the provider callers' existing HTTP-error policy.
// headersOnly closes at the first response headers, preserving the webhook
// sender's status-only behavior without buffering an unused response body.
HttpExchange exchange(const QUrl &url, const QMap<QByteArray, QByteArray> &headers,
                      double timeoutSeconds, qsizetype maximumBytes, const Cancellation &cancelled,
                      const std::optional<QByteArray> &payload = {}, bool allowHttpErrors = false,
                      bool headersOnly = false, bool head = false);

// Shared worker-owned I/O; provider and metadata callers retain their distinct
// URL, authentication, MIME, size and display-error policies.
HttpResponse get(const QUrl &url, const QMap<QByteArray, QByteArray> &headers,
                 double timeoutSeconds, qsizetype maximumBytes, const Cancellation &cancelled);

} // namespace introvert::providers::detail
