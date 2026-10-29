#pragma once
#include "process_runner.h"
#include <QThread>
#include <QVariantMap>
#include <condition_variable>

namespace introvert::detection {
struct MemorySnapshot { qint64 physical=0, available=-1; };
MemorySnapshot systemMemory();
QString effortName(const QString &name);
struct ResourceProfile {
    QString name;
    int matchingWorkers, decoderThreads, mediaWorkers, visualWorkers;
    qint64 pcmDiskBytes, workingBytes, memoryReserveBytes;
    int logicalProcessors, cpuBudget;
    MemorySnapshot memory;
    QThread::Priority priority;
    QThread::QualityOfService serviceLevel;
    QVariantMap diagnostics() const;
};
ResourceProfile resourceProfile(const QString &name, int logicalProcessors = QThread::idealThreadCount(), MemorySnapshot memory = systemMemory());
// Admission reservations bound concurrency, not third-party process RSS. New
// work pauses under memory pressure; cancellation remains responsive.
class WorkAdmission final {
public:
    using Reader=std::function<MemorySnapshot()>;
    WorkAdmission(qint64 budget,qint64 reserve,Reader reader=systemMemory):m_budget(budget),m_reserve(reserve),m_reader(std::move(reader)){}
    class Lease {
    public:
        ~Lease();
        Lease(const Lease &)=delete;
    private:
        friend class WorkAdmission;
        Lease(WorkAdmission *owner,qint64 bytes):owner(owner),bytes(bytes){}
        WorkAdmission *owner; qint64 bytes;
    };
    std::unique_ptr<Lease> acquire(qint64 bytes,const Cancellation &cancel={});
    QVariantMap diagnostics() const;
private:
    qint64 m_budget,m_reserve,m_used=0,m_peak=0,m_pressureWaits=0;
    Reader m_reader; mutable std::mutex m_mutex; std::condition_variable m_changed;
};
} // namespace introvert::detection
