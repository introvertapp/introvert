#pragma once
#include <QElapsedTimer>
#include <QVariantMap>
#include <ctime>
namespace introvert::detection {
// Application-process counters, not an attribution of all CPU work to detection.
// Child CPU is available on Unix and includes reaped application children.
class JobMetrics final {
public:
    JobMetrics();
    QVariantMap diagnostics() const;
private:
    QElapsedTimer m_wall;
    std::clock_t m_cpu;
    double m_childCpu=0;
};
}
