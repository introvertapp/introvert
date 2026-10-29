#pragma once

#include "contracts.h"
#include <QByteArray>
#include <QMap>
#include <QSemaphore>
#include <QStringList>
#include <QVariantMap>
#include <memory>
#include <mutex>

namespace introvert::detection {

struct ResourceLimits {
    int probes = 4, decoders = 2, application = 6, job = 4;
    double timeoutSeconds = 300, terminationGraceSeconds = .5;
    qint64 maximumStdoutBytes = 8 * 1024 * 1024, maximumStderrBytes = 256 * 1024;
    void validate() const;
};
struct ProcessRequest {
    QStringList arguments;
    ProcessCategory category;
    QString componentId, jobId;
    std::optional<QString> episodeId;
    std::optional<SegmentType> segmentType;
    std::optional<double> timeoutSeconds;
    qint64 maximumStreamBytes = 0;
    std::function<void(const QByteArray &)> stdoutConsumer;
    void validate() const;
};
struct ProcessResult {
    std::optional<int> exitCode;
    QByteArray out, error;
    double runtimeSeconds = 0;
    bool timedOut = false, cancelled = false;
    bool outputLimitExceeded = false;
    QString cleanupStatus = "not_started";
    double queueSeconds = 0;
};
struct ProcessDiagnostic { ProcessRequest request; ProcessResult result; };

class ProcessRunner {
public:
    virtual ~ProcessRunner() = default;
    virtual ProcessResult run(const ProcessRequest &request, const Cancellation &cancel = {}) = 0;
    virtual QList<ProcessDiagnostic> diagnostics() const { return {}; }
};

// Synchronous worker boundary, shared across a composition. Every QProcess is
// created and destroyed on its caller's thread; no shell is involved.
class ResourceManager final : public ProcessRunner {
public:
    explicit ResourceManager(ResourceLimits limits = {});
    ProcessResult run(const ProcessRequest &request, const Cancellation &cancel = {}) override;
    QList<ProcessDiagnostic> diagnostics() const override;
    QVariantMap statistics() const;
private:
    ProcessResult record(const ProcessRequest &request, ProcessResult result);
    ResourceLimits m_limits;
    QSemaphore m_application, m_probes, m_decoders;
    QMap<QString, std::shared_ptr<QSemaphore>> m_jobs;
    mutable std::mutex m_mutex;
    QList<ProcessDiagnostic> m_diagnostics;
    int m_activeDecoders=0,m_activeProbes=0,m_peakDecoders=0,m_peakProbes=0;
};

} // namespace introvert::detection
