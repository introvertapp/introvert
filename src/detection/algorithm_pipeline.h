#pragma once
#include "algorithm_client.h"
#include "algorithm_session.h"

namespace introvert::detection {
// Internal adapter on each side of the C boundary. No C++ object crosses it.
class PipelineServices final {
public:
    explicit PipelineServices(const IntrovertDetectorServicesV1 &host) : m_host(host) {}
    void check() const;
    QVariantMap request(const char *operation, const QVariantMap &input = {}) const;
    void publish(const QVariantMap &value) const;
    int64_t readInto(const QString &episode, SegmentType type, uint32_t rate,
        const TimeRange &window, const TimeRange &part, qint64 first, uint32_t count, int16_t *output) const;
    class Lease final {
    public:
        Lease(const PipelineServices &services, const char *operation, const QVariantMap &input);
        ~Lease();
        Lease(const Lease &) = delete;
        qint64 id() const { return m_id; }
    private:
        const PipelineServices &m_services;
        qint64 m_id;
    };
private:
    const IntrovertDetectorServicesV1 &m_host;
};
// Shared source retains compatibility with already installed algorithm-only modules.
// Session-capable modules execute their own copy; the host supplies only services.
void runAlgorithmPipeline(Algorithm &algorithm, const QVariantMap &input, const PipelineServices &services);
// An unsuccessful review may retain an existing audio selection, but cannot
// promote an unconfirmed hypothesis or restore a quarantined gross outlier.
QVariantMap acceptedBoundarySelection(const QVariantMap &original,const QVariantMap &refined);
}
