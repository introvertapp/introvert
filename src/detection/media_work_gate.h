#pragma once
#include "contracts.h"
#include <condition_variable>
#include <memory>
#include <mutex>

namespace introvert::detection {
// A server that fails under parallel reads can reduce its input concurrency
// without reducing the CPU worker pool or interrupting existing readers.
class MediaWorkGate final {
public:
    explicit MediaWorkGate(int limit) : m_limit(limit) {
        if(limit<1)throw std::invalid_argument("Invalid media worker budget.");
    }
    class Lease final {
    public:
        explicit Lease(MediaWorkGate &gate) : m_gate(gate) {}
        ~Lease() {std::lock_guard<std::mutex> lock(m_gate.m_mutex);--m_gate.m_active;m_gate.m_changed.notify_all();}
        Lease(const Lease &)=delete;
        Lease &operator=(const Lease &)=delete;
    private:
        MediaWorkGate &m_gate;
    };
    std::unique_ptr<Lease> acquire(const Cancellation &cancel={}) {
        std::unique_lock<std::mutex> lock(m_mutex);
        while(m_active>=m_limit) {checkCancelled(cancel);m_changed.wait_for(lock,std::chrono::milliseconds(20));}
        checkCancelled(cancel);
        auto lease=std::make_unique<Lease>(*this);++m_active;return lease;
    }
    void reduceToOne() {std::lock_guard<std::mutex> lock(m_mutex);m_limit=1;m_changed.notify_all();}
    int limit() const {std::lock_guard<std::mutex> lock(m_mutex);return m_limit;}
private:
    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    int m_limit,m_active=0;
};
} // namespace introvert::detection
