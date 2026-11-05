#pragma once

#include "endpoint_client.h"

namespace introvert::submissions {

struct SubmissionClaim { QVariantMap job, item; };
struct SubmissionSnapshot { QVariantList queue, history; QVariantMap statistics; };
struct SubmissionCompletion {
    QString status;
    std::optional<QString> comparison;
    QString error;
    std::optional<int> httpStatus;
    std::optional<SubmissionResult> limit;
    std::optional<QVariantMap> settings;
};

// SQLite owns all queue/rate-limit/history state. One worker per endpoint calls
// nextItem/processOne with its endpoint; enqueues use independent SQL connections and
// revisions protect items changed while an endpoint request is in flight.
class SubmissionStore final {
public:
    using Clock = EndpointClient::Clock;
    explicit SubmissionStore(QString databasePath, Clock clock = {});
    double currentTime() const { return m_clock(); }
    void initialize() const;
    QString enqueue(const QVariantMap &plan) const;
    QStringList enqueueMany(const QVariantList &plans) const;
    bool moveQueueItem(const QString &key, const QString &target, bool after, const QStringList &order = {}) const;
    std::optional<SubmissionClaim> nextItem(const QString &endpoint = {}) const;
    void finish(const SubmissionClaim &claim, const SubmissionCompletion &completion) const;
    bool processOne(const SettingsStore &settings, const providers::Cancellation &cancelled,
                    const std::function<void()> &progress = {}, const QString &endpoint = {}) const;
    SubmissionSnapshot snapshot() const;
    QVariantMap episodeStatuses(bool includeActivity = false, const QString &scope = {}, const QStringList &canonical = {}) const;
private:
    QString enqueuePlan(const QVariantMap &plan, QSqlDatabase &db) const;
    QString m_path;
    Clock m_clock;
};

} // namespace introvert::submissions
