#include "pcm_cache.h"
#include <algorithm>
#include <cmath>
namespace introvert::detection {
PcmCache::Lease::~Lease()
{
    std::lock_guard<std::mutex> lock(m_owner->m_mutex);--m_entry->pins;m_owner->m_changed.notify_all();
}
const AudioWindow &PcmCache::Lease::window() const {return m_entry->audio->window();}
void PcmCache::makeRoom(qint64 bytes,std::unique_lock<std::mutex> &lock,const Cancellation &cancel)
{
    if(bytes<=0 || bytes>m_limit)throw DetectionError("algorithm_audio_cache_limit","Scoped PCM exceeds its work-storage budget.");
    while(bytes>m_limit-m_bytes) {
        checkCancelled(cancel);auto oldest=m_entries.end();
        for(auto it=m_entries.begin();it!=m_entries.end();++it)
            if(!it->second->loading && !it->second->pins && (oldest==m_entries.end() || it->second->used<oldest->second->used))oldest=it;
        if(oldest!=m_entries.end()){m_bytes-=oldest->second->bytes;m_entries.erase(oldest);}
        else m_changed.wait_for(lock,std::chrono::milliseconds(20));
    }
}
bool PcmCache::retain(const Key &key,DecodedAudio &&audio,const Cancellation &cancel)
{
    const auto bytes=audio.window().byteCount;
    if(bytes>m_limit || std::abs(audio.window().durationDifference)>.05)return false;
    auto entry=std::make_shared<Entry>();entry->audio=std::make_unique<DecodedAudio>(std::move(audio));entry->bytes=bytes;entry->loading=false;
    std::unique_lock<std::mutex> lock(m_mutex);if(m_entries.count(key))return false;
    makeRoom(bytes,lock,cancel);checkCancelled(cancel);
    if(m_entries.count(key))return false;
    entry->used=++m_clock;m_entries.emplace(key,entry);
    m_bytes+=bytes;m_peak=std::max(m_peak,m_bytes);++m_prefetched;return true;
}
std::unique_ptr<PcmCache::Lease> PcmCache::retained(const Key &key)
{
    std::lock_guard<std::mutex> lock(m_mutex);const auto found=m_entries.find(key);
    if(found==m_entries.end() || found->second->loading || found->second->error)return {};
    auto lease=std::unique_ptr<Lease>(new Lease(this,found->second));
    ++found->second->pins;found->second->used=++m_clock;++m_hits;return lease;
}
std::unique_ptr<PcmCache::Lease> PcmCache::get(const Key &key,qint64 reservation,const Loader &load,const Cancellation &cancel)
{
    std::unique_lock<std::mutex> lock(m_mutex);checkCancelled(cancel);
    auto found=m_entries.find(key);
    if(found!=m_entries.end()) {
        auto entry=found->second;
        ++entry->pins;
        try {
            while(entry->loading){m_changed.wait_for(lock,std::chrono::milliseconds(20));checkCancelled(cancel);}
            if(entry->error)std::rethrow_exception(entry->error);
            auto lease=std::unique_ptr<Lease>(new Lease(this,entry));entry->used=++m_clock;++m_hits;return lease;
        } catch(...){--entry->pins;m_changed.notify_all();throw;}
    }
    auto entry=std::make_shared<Entry>();makeRoom(reservation,lock,cancel);checkCancelled(cancel);
    // A wait for budget can let another worker insert this key.
    if(m_entries.count(key)){lock.unlock();return get(key,reservation,load,cancel);}
    entry->bytes=reservation;entry->used=++m_clock;m_entries.emplace(key,entry);m_bytes+=reservation;m_peak=std::max(m_peak,m_bytes);
    ++m_loads;++m_activeLoads;m_peakLoads=std::max(m_peakLoads,m_activeLoads);lock.unlock();
    std::unique_ptr<DecodedAudio> audio;std::exception_ptr error;
    try {
        audio=std::make_unique<DecodedAudio>(load());checkCancelled(cancel);
        if(audio->window().byteCount>reservation || std::abs(audio->window().durationDifference)>.05)
            throw DetectionError("algorithm_incomplete_audio_read","Refinement audio does not cover its hard window.");
    } catch(...){error=std::current_exception();}
    lock.lock();--m_activeLoads;m_bytes-=reservation;entry->loading=false;entry->error=error;
    if(!error){entry->bytes=audio->window().byteCount;entry->audio=std::move(audio);m_bytes+=entry->bytes;entry->used=++m_clock;}
    else entry->bytes=0;
    m_changed.notify_all();if(error)std::rethrow_exception(error);
    auto lease=std::unique_ptr<Lease>(new Lease(this,entry));++entry->pins;return lease;
}
QVariantMap PcmCache::statistics() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return {{"pcm_prefetched",m_prefetched},{"pcm_decodes",m_loads},{"pcm_cache_hits",m_hits},{"peak_pcm_disk_bytes",m_peak},
        {"pcm_disk_budget",m_limit},{"peak_parallel_pcm_loads",m_peakLoads}};
}
} // namespace introvert::detection
