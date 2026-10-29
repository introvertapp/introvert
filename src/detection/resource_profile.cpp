#include "resource_profile.h"
#include <QFile>
#include <algorithm>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(Q_OS_MACOS)
#include <sys/sysctl.h>
#include <mach/mach.h>
#elif defined(Q_OS_UNIX)
#include <unistd.h>
#endif
namespace introvert::detection {
namespace { constexpr qint64 MiB=1024*1024,GiB=1024*MiB; }
MemorySnapshot systemMemory()
{
    MemorySnapshot m;
#ifdef Q_OS_WIN
    MEMORYSTATUSEX s{};s.dwLength=sizeof(s);
    if(GlobalMemoryStatusEx(&s)){m.physical=qint64(s.ullTotalPhys);m.available=qint64(s.ullAvailPhys);}
#elif defined(Q_OS_MACOS)
    uint64_t total=0;size_t size=sizeof(total);
    if(sysctlbyname("hw.memsize",&total,&size,nullptr,0)==0)m.physical=qint64(total);
    vm_statistics64_data_t stats{};mach_msg_type_number_t count=HOST_VM_INFO64_COUNT;
    const auto host=mach_host_self();vm_size_t page=0;
    if(host_page_size(host,&page)==KERN_SUCCESS && host_statistics64(host,HOST_VM_INFO64,reinterpret_cast<host_info64_t>(&stats),&count)==KERN_SUCCESS)
        m.available=(qint64(stats.free_count)+qint64(stats.inactive_count))*qint64(page);
    mach_port_deallocate(mach_task_self(),host);
#elif defined(Q_OS_LINUX)
    QFile file("/proc/meminfo");if(file.open(QIODevice::ReadOnly))for(const auto &line:file.readAll().split('\n')) {
        const auto parts=line.simplified().split(' ');
        if(parts.size()>=2 && parts[0]=="MemTotal:")m.physical=parts[1].toLongLong()*1024;
        if(parts.size()>=2 && parts[0]=="MemAvailable:")m.available=parts[1].toLongLong()*1024;
    }
#endif
    if(m.physical>0 && m.available>m.physical)m.available=m.physical;
    return m;
}
QString effortName(const QString &name)
{
    if(name=="low" || name=="light")return "light";
    if(name=="medium")return name;
    if(name=="full" || name=="high")return "high";
    throw std::invalid_argument("Unsupported detection effort.");
}
ResourceProfile resourceProfile(const QString &input,int processors,MemorySnapshot memory)
{
    const auto name=effortName(input);const int cores=std::clamp(processors,1,256);
    const qint64 physical=memory.physical>0?memory.physical:4*GiB;
    const qint64 available=memory.available>=0?memory.available:std::min(physical/2,2*GiB);
    const qint64 reserve=std::min(physical/4,std::max(256*MiB,physical/10));
    const qint64 spare=std::max(qint64(0),available-reserve);
    const int cpu=name=="light"?1:name=="medium"?std::max(1,std::min(8,cores/2)):std::max(1,cores-(cores>8?2:1));
    const qint64 ceiling=name=="light"?256*MiB:name=="medium"?2*GiB:8*GiB;
    const qint64 fraction=name=="light"?available/8:name=="medium"?available/3:available/4*3;
    const qint64 working=std::max(qint64(1),std::min({ceiling,physical/2,fraction,spare}));
    const int matching=std::max(1,std::min(cpu,int(working/(32*MiB))));
    const int decoders=name=="light"?1:2;
    const int media=std::max(1,std::min({name=="light"?1:name=="medium"?2:32,std::max(1,cpu/decoders),int(working/(256*MiB))}));
    const int visual=std::max(1,std::min({name=="high"?8:1,std::max(1,cpu/2),int(working/(512*MiB))}));
    return {name,matching,decoders,media,visual,name=="light"?128*MiB:name=="medium"?512*MiB:4*GiB,
        working,reserve,cores,cpu,memory,name=="light"?QThread::LowPriority:QThread::NormalPriority,
        name=="light"?QThread::QualityOfService::Eco:name=="high"?QThread::QualityOfService::High:QThread::QualityOfService::Auto};
}
QVariantMap ResourceProfile::diagnostics() const
{
    return {{"profile",name},{"policy","effort-resources-v1"},{"matching_workers",matchingWorkers},{"decoder_threads",decoderThreads},
        {"media_workers",mediaWorkers},{"visual_workers",visualWorkers},{"pcm_disk_budget",pcmDiskBytes},{"working_reservation_budget",workingBytes},
        {"memory_reserve",memoryReserveBytes},{"physical_memory",memory.physical},{"available_memory_at_start",memory.available},
        {"logical_processors",logicalProcessors},{"cpu_budget",cpuBudget},{"scope","application-detection"},{"priority",name=="light"?"background":"normal"}};
}
WorkAdmission::Lease::~Lease()
{
    std::lock_guard<std::mutex> lock(owner->m_mutex);owner->m_used-=bytes;owner->m_changed.notify_all();
}
std::unique_ptr<WorkAdmission::Lease> WorkAdmission::acquire(qint64 bytes,const Cancellation &cancel)
{
    if(bytes<=0)throw std::invalid_argument("Invalid work reservation.");
    const auto reservation=std::min(bytes,m_budget);
    std::unique_lock<std::mutex> lock(m_mutex);
    for(;;) {
        checkCancelled(cancel); const auto memory=m_reader();
        // Keep the reserve even when this job is otherwise idle. Already-running
        // work drains normally; waiting never blocks the GUI or cancellation.
        const bool pressure=memory.available>=0 && memory.available-m_reserve<reservation;
        if(!pressure && reservation<=m_budget-m_used)break;
        if(pressure)++m_pressureWaits;
        m_changed.wait_for(lock,std::chrono::milliseconds(50));
    }
    auto lease=std::unique_ptr<Lease>(new Lease(this,reservation));
    m_used+=reservation;m_peak=std::max(m_peak,m_used);return lease;
}
QVariantMap WorkAdmission::diagnostics() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return {{"peak_reserved_bytes",m_peak},{"reservation_budget",m_budget},{"pressure_waits",m_pressureWaits},{"rss_hard_limit",false}};
}
} // namespace introvert::detection
