#include "process_runner.h"
#include <QElapsedTimer>
#include <QProcess>
#include <algorithm>
#include <cmath>
#include <limits>

namespace introvert::detection {
namespace {
bool cancelled(const Cancellation &cancel)
{
    try { checkCancelled(cancel); return false; } catch (const Cancelled &) { return true; }
}
ResourceLimits validated(ResourceLimits limits) { limits.validate(); return limits; }
struct Permits {
    QList<QSemaphore *> held;
    ~Permits() { for (auto it = held.crbegin(); it != held.crend(); ++it) (*it)->release(); }
};
}

void ResourceLimits::validate() const
{
    if (probes <= 0 || decoders <= 0 || application <= 0 || job <= 0) throw std::invalid_argument("process limits must be positive");
    if (maximumStdoutBytes <= 0 || maximumStderrBytes <= 0) throw std::invalid_argument("process output limits must be positive");
    if (!std::isfinite(timeoutSeconds) || timeoutSeconds <= 0 || !std::isfinite(terminationGraceSeconds) || terminationGraceSeconds < 0)
        throw std::invalid_argument("timeout values must be valid");
}
void ProcessRequest::validate() const
{
    if (arguments.isEmpty() || std::any_of(arguments.cbegin(), arguments.cend(), [](const QString &argument) { return argument.isEmpty(); }))
        throw std::invalid_argument("process arguments must be a non-empty string tuple");
    if (componentId.isEmpty() || jobId.isEmpty()) throw std::invalid_argument("component and job IDs are required");
    if (timeoutSeconds && (!std::isfinite(*timeoutSeconds) || *timeoutSeconds <= 0)) throw std::invalid_argument("process timeout must be positive");
    if (stdoutConsumer && maximumStreamBytes <= 0) throw std::invalid_argument("streamed output requires a byte budget");
}

ResourceManager::ResourceManager(ResourceLimits limits) : m_limits(validated(limits)), m_application(m_limits.application),
    m_probes(m_limits.probes), m_decoders(m_limits.decoders) {}

ProcessResult ResourceManager::record(const ProcessRequest &request, ProcessResult result)
{
    // Diagnostics must not retain raw paths, command arguments, stdout or stderr.
    auto identity = request; identity.arguments.clear(); identity.stdoutConsumer = {};
    auto summary = result; summary.out.clear(); summary.error.clear();
    std::lock_guard<std::mutex> lock(m_mutex); m_diagnostics.append({identity, summary}); return result;
}
QList<ProcessDiagnostic> ResourceManager::diagnostics() const { std::lock_guard<std::mutex> lock(m_mutex); return m_diagnostics; }

QVariantMap ResourceManager::statistics() const
{
    std::lock_guard<std::mutex> lock(m_mutex); QVariantMap components;
    for(const auto &d:m_diagnostics) {
        auto value=components.value(d.request.componentId).toMap();
        value["processes"]=value.value("processes").toInt()+1;
        value["runtime_ms"]=value.value("runtime_ms").toDouble()+d.result.runtimeSeconds*1000.;
        value["queue_wait_ms"]=value.value("queue_wait_ms").toDouble()+d.result.queueSeconds*1000.;
        value["failed"]=value.value("failed").toInt()+(d.result.cancelled || d.result.timedOut || d.result.outputLimitExceeded || d.result.exitCode!=0);
        components[d.request.componentId]=value;
    }
    return {{"peak_active_decoders",m_peakDecoders},{"peak_active_probes",m_peakProbes},{"components",components}};
}

ProcessResult ResourceManager::run(const ProcessRequest &request, const Cancellation &cancel)
{
    request.validate(); QElapsedTimer started; started.start(); ProcessResult result;
    std::shared_ptr<QSemaphore> job;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto &slot = m_jobs[request.jobId]; if (!slot) slot = std::make_shared<QSemaphore>(m_limits.job); job = slot;
    }
    const auto notStarted = [&](const QByteArray &reason) {
        result.cancelled = true; result.error = reason; result.queueSeconds = started.nsecsElapsed() / 1e9; return record(request, result);
    };
    if (cancelled(cancel)) return notStarted("cancelled before launch");
    Permits permits;
    for (auto *semaphore : {&m_application, job.get(), request.category == ProcessCategory::Probe ? &m_probes : &m_decoders}) {
        while (!semaphore->tryAcquire(1, 10)) if (cancelled(cancel)) return notStarted("cancelled while queued");
        permits.held.append(semaphore);
    }
    if (cancelled(cancel)) return notStarted("cancelled before launch");
    result.queueSeconds=started.nsecsElapsed()/1e9;
    {std::lock_guard<std::mutex> lock(m_mutex); if(request.category==ProcessCategory::Decoder)m_peakDecoders=std::max(m_peakDecoders,++m_activeDecoders); else m_peakProbes=std::max(m_peakProbes,++m_activeProbes);}
    struct Active {std::function<void()> finish; ~Active(){finish();}} active{[&]{std::lock_guard<std::mutex> lock(m_mutex); if(request.category==ProcessCategory::Decoder)--m_activeDecoders; else --m_activeProbes;}};
    QProcess process; process.setProcessChannelMode(QProcess::SeparateChannels);
    process.setProgram(request.arguments.first()); process.setArguments(request.arguments.mid(1));
    started.restart(); process.start();
    const auto drainChannel = [&](QProcess::ProcessChannel channel, QByteArray &bytes, qint64 maximum) {
        process.setReadChannel(channel);
        while (process.bytesAvailable() > 0) {
            const auto available = maximum - bytes.size();
            if (available <= 0) { result.outputLimitExceeded = true; break; }
            bytes.append(process.read(std::min(qint64(64 * 1024), available)));
        }
    };
    qint64 streamed = 0;
    const auto drain = [&] {
        if (request.stdoutConsumer && !result.cancelled && !result.timedOut) {
            process.setReadChannel(QProcess::StandardOutput);
            while (process.bytesAvailable() > 0) {
                checkCancelled(cancel);
                if (started.nsecsElapsed()/1e9 >= request.timeoutSeconds.value_or(m_limits.timeoutSeconds)) { result.timedOut=true; break; }
                const auto bytes = process.read(64 * 1024);
                if (bytes.size() > request.maximumStreamBytes - streamed) { result.outputLimitExceeded = true; break; }
                streamed += bytes.size(); request.stdoutConsumer(bytes);
            }
        } else if (!request.stdoutConsumer) drainChannel(QProcess::StandardOutput, result.out, m_limits.maximumStdoutBytes);
        drainChannel(QProcess::StandardError, result.error, m_limits.maximumStderrBytes);
    };
    try {
        const auto timeout = request.timeoutSeconds.value_or(m_limits.timeoutSeconds);
        bool launched = process.waitForStarted(10);
        while (!launched && process.state() == QProcess::Starting) {
            result.cancelled = cancelled(cancel); result.timedOut = !result.cancelled && started.nsecsElapsed() / 1e9 >= timeout;
            if (result.cancelled || result.timedOut) break;
            launched = process.waitForStarted(10);
        }
        if (!launched && !result.cancelled && !result.timedOut) throw std::runtime_error("Detector process could not be launched.");
        while (process.state() != QProcess::NotRunning) {
            drain(); result.cancelled = cancelled(cancel);
            result.timedOut = !result.cancelled && started.nsecsElapsed() / 1e9 >= timeout;
            if (!result.cancelled && !result.timedOut && !result.outputLimitExceeded) { process.waitForFinished(10); continue; }
            process.terminate();
            const auto grace = int(std::clamp(m_limits.terminationGraceSeconds * 1000, 0., double(std::numeric_limits<int>::max())));
            if (process.waitForFinished(grace)) result.cleanupStatus = "terminated";
            else { process.kill(); process.waitForFinished(-1); result.cleanupStatus = "killed"; }
            break;
        }
    } catch (...) {process.kill();process.waitForFinished(-1);result.runtimeSeconds=started.nsecsElapsed()/1e9;result.cleanupStatus="exception";record(request,result);throw;}
    drain(); result.runtimeSeconds = started.nsecsElapsed() / 1e9;
    result.exitCode = process.exitCode();
#ifndef Q_OS_WIN
    if (process.exitStatus() == QProcess::CrashExit && *result.exitCode > 0) result.exitCode = -*result.exitCode;
#endif
    if (!result.timedOut && !result.cancelled && result.cleanupStatus == "not_started") result.cleanupStatus = "completed";
    return record(request, result);
}

} // namespace introvert::detection
