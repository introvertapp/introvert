#pragma once
#include "contracts.h"
#include <QRunnable>
#include <QSemaphore>
#include <QThreadPool>
#include <QVariantMap>
#include <atomic>
#include <algorithm>
#include <future>
#include <vector>

namespace introvert::detection {
// Bounded lookahead with deterministic consumption. A worker immediately takes
// another task when space is available; no batch waits for its slowest member.
// The owning scope must outlive every object captured by work.
template<class Result> class ContinuousWork final {
public:
    template<class Work> ContinuousWork(size_t count,int workers,QThread::Priority priority,QThread::QualityOfService qos,Work work)
        : m_promises(count),m_slots(std::clamp(workers,1,256)*2)
    {
        if(workers<1 || workers>256)throw std::invalid_argument("Invalid worker budget.");
        m_pool.setMaxThreadCount(workers); m_pool.setThreadPriority(priority); m_pool.setServiceLevel(qos);
        for (auto &promise:m_promises) m_results.push_back(promise.get_future());
        try {
        for (size_t i=0;i<std::min(count,size_t(workers));++i) m_pool.start(QRunnable::create([this,count,work] {
            while (!m_stopping.load()) {
                while (!m_slots.tryAcquire(1,20)) if (m_stopping.load())return;
                if (m_stopping.load())return;
                const auto n=m_next.fetch_add(1); if(n>=count)return;
                const int active=++m_active; auto peak=m_peak.load(); while(active>peak && !m_peak.compare_exchange_weak(peak,active)){}
                bool finished=false;
                const auto finish=[&] {if(!finished) {--m_active;++m_completed;finished=true;}};
                try {
                    auto result=work(n); finish(); m_promises[n].set_value(std::move(result));
                } catch (...) {finish(); m_promises[n].set_exception(std::current_exception());}
            }
        }));
        } catch (...) {m_stopping.store(true);m_pool.waitForDone();throw;}
    }
    ~ContinuousWork() {m_stopping.store(true); m_pool.waitForDone();}
    Result take(size_t index,const Cancellation &cancel={}) {
        auto &future=m_results.at(index);
        while(future.wait_for(std::chrono::milliseconds(20))!=std::future_status::ready)checkCancelled(cancel);
        try {auto result=future.get();m_slots.release();return result;}
        catch (...) {m_slots.release();throw;}
    }
    QVariantMap diagnostics() const {return {{"workers",m_pool.maxThreadCount()},{"peak_active",m_peak.load()},{"completed",qulonglong(m_completed.load())},{"lookahead",m_pool.maxThreadCount()*2}};}
private:
    std::vector<std::promise<Result>> m_promises;
    std::vector<std::future<Result>> m_results;
    QSemaphore m_slots;
    std::atomic_size_t m_next{0},m_completed{0};
    std::atomic_int m_active{0},m_peak{0}; std::atomic_bool m_stopping{false};
    QThreadPool m_pool;
};
} // namespace introvert::detection
