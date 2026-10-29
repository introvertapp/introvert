#include "job_metrics.h"
#ifndef Q_OS_WIN
#include <sys/resource.h>
#endif
namespace introvert::detection {
namespace {
double childrenCpu() {
#ifndef Q_OS_WIN
    rusage usage{}; if(getrusage(RUSAGE_CHILDREN,&usage)==0)
        return usage.ru_utime.tv_sec+usage.ru_utime.tv_usec/1e6+usage.ru_stime.tv_sec+usage.ru_stime.tv_usec/1e6;
#endif
    return 0;
}
}
JobMetrics::JobMetrics():m_cpu(std::clock()),m_childCpu(childrenCpu()){m_wall.start();}
QVariantMap JobMetrics::diagnostics() const {
    const double wall=m_wall.nsecsElapsed()/1e9,self=double(std::clock()-m_cpu)/CLOCKS_PER_SEC,child=childrenCpu()-m_childCpu;
    QVariantMap d{{"scope","application-process-and-reaped-children"},{"wall_seconds",wall},{"process_cpu_seconds",self},
        {"average_process_cpu_cores",wall>0?self/wall:0.},{"gpu_utilization_available",false},{"process_cpu_available",true}};
#ifndef Q_OS_WIN
    d["reaped_children_cpu_seconds"]=child; d["average_process_and_children_cpu_cores"]=wall>0?(self+child)/wall:0.;
    rusage usage{}; if(getrusage(RUSAGE_SELF,&usage)==0) {
#ifdef Q_OS_MACOS
        d["process_lifetime_peak_rss_bytes"]=qlonglong(usage.ru_maxrss);
#else
        d["process_lifetime_peak_rss_bytes"]=qlonglong(usage.ru_maxrss)*1024;
#endif
    }
#else
    // MSVC's clock() measures wall time. Do not present it as CPU telemetry.
    d["process_cpu_available"]=false; d.remove("process_cpu_seconds"); d.remove("average_process_cpu_cores");
    d["child_cpu_available"]=false;
#endif
    return d;
}
}
