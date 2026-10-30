#pragma once

#include "core/settings_store.h"
#include "providers/contracts.h"
#include <QJsonObject>
#include <functional>

namespace introvert::submissions {

class EndpointError final : public std::runtime_error {
public:
    explicit EndpointError(const char *message) : std::runtime_error(message) {}
};
class SubmissionError final : public std::invalid_argument {
public:
    explicit SubmissionError(const char *message) : std::invalid_argument(message) {}
};
struct SubmissionResult {
    QString result;
    std::optional<int> httpStatus;
    QString error;
    std::optional<QString> resetAt, resumeAt;
};
struct SegmentMatch {
    double startSeconds;
    std::optional<double> endSeconds;
    QVariant confidence, submissionCount, updatedAt;
};
struct LookupResult {
    QList<SegmentMatch> matches;
    std::optional<SubmissionResult> failure;
    bool found() const { return !matches.isEmpty(); }
};

// Immutable worker snapshot. No retries, sleeps, queue or implicit requests;
// credentials have no display/debug/JSON representation.
class EndpointClient final {
public:
    using Clock = std::function<double()>;
    EndpointClient(QString endpoint, QVariantMap settings, std::optional<QString> apiKey = {}, Clock clock = {});
    static EndpointClient fromStore(const QString &endpoint, const SettingsStore &store, Clock clock = {});
    bool enabled() const;
    bool configured() const { return enabled() && m_key && !m_key->isEmpty(); }
    LookupResult lookup(const QVariantMap &values, const providers::Cancellation &cancelled = {}) const;
    void validate(const QVariantMap &values) const;
    QVariantMap resolve(const QVariantMap &values, const providers::Cancellation &cancelled = {}) const;
    SubmissionResult submit(const QVariantMap &values, const providers::Cancellation &cancelled = {}) const;
    QString endpoint() const { return m_endpoint; }
    QVariantMap settings() const { return m_settings; }
    static std::optional<QString> resetDate(const QVariant &value, double now, bool relative = false);
private:
    struct Payload { QString url; QJsonObject body; };
    struct Response { int status; QMap<QByteArray, QByteArray> headers; QJsonValue body; };
    SubmissionResult rateLimit(const Response &response) const;
    Payload payload(const QVariantMap &values, bool requireCredential = true) const;
    Response request(QString url, const QVariantMap &query, const std::optional<QJsonObject> &body,
                     bool authenticated, const providers::Cancellation &cancelled) const;
    QString m_endpoint;
    QVariantMap m_settings;
    std::optional<QString> m_key;
    Clock m_clock;
};

QString comparison(const LookupResult &lookup, double start, const std::optional<double> &end);

} // namespace introvert::submissions
