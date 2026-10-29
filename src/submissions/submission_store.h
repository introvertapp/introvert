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

// SQLite owns all queue/rate-limit/history state. One application worker calls
// nextItem/processOne; concurrent enqueues use independent SQL connections and
// revisions protect items changed while an endpoint request is in flight.
class SubmissionStore final {
public:
    using Clock = EndpointClient::Clock;
    explicit SubmissionStore(QString databasePath, Clock clock = {});
    void initialize() const;
    QString enqueue(const QVariantMap &plan) const;
    std::optional<SubmissionClaim> nextItem() const;
    void finish(const SubmissionClaim &claim, const SubmissionCompletion &completion) const;
    bool processOne(const SettingsStore &settings, const providers::Cancellation &cancelled,
                    const std::function<void()> &progress = {}) const;
    SubmissionSnapshot snapshot() const;
    QVariantMap episodeStatuses() const;
private:
    QString m_path;
    Clock m_clock;
};

} // namespace introvert::submissions
