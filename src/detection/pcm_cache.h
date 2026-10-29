#pragma once
#include "audio_decode.h"
#include <condition_variable>
#include <map>
#include <tuple>

namespace introvert::detection {
// Job-owned, bounded PCM storage. Independent keys decode concurrently; readers
// of the same key share one load. Pinned files cannot be evicted during a read.
class PcmCache final {
    struct Entry;
public:
    struct Key {
        QString episode;SegmentType type;uint32_t rate;qint64 first=-1,last=-1;
        bool operator<(const Key &other) const {return std::tie(episode,type,rate,first,last)<std::tie(other.episode,other.type,other.rate,other.first,other.last);}
    };
    using Loader=std::function<DecodedAudio()>;
    explicit PcmCache(qint64 limit):m_limit(limit){if(limit<=0)throw std::invalid_argument("Invalid PCM budget.");}
    class Lease final {
    public:
        ~Lease();
        const AudioWindow &window() const;
        Lease(const Lease &)=delete;
    private:
        friend class PcmCache;
        Lease(PcmCache *owner,std::shared_ptr<Entry> entry):m_owner(owner),m_entry(std::move(entry)){}
        PcmCache *m_owner;std::shared_ptr<Entry> m_entry;
    };
    bool retain(const Key &key,DecodedAudio &&audio,const Cancellation &cancel={});
    std::unique_ptr<Lease> retained(const Key &key);
    std::unique_ptr<Lease> get(const Key &key,qint64 reservation,const Loader &load,const Cancellation &cancel={});
    QVariantMap statistics() const;
private:
    struct Entry {bool loading=true;std::exception_ptr error;std::unique_ptr<DecodedAudio> audio;qint64 bytes=0;int pins=0;quint64 used=0;};
    void makeRoom(qint64 bytes,std::unique_lock<std::mutex> &lock,const Cancellation &cancel);
    std::map<Key,std::shared_ptr<Entry>> m_entries;
    const qint64 m_limit;
    qint64 m_bytes=0,m_peak=0,m_loads=0,m_hits=0,m_prefetched=0;quint64 m_clock=0;
    int m_activeLoads=0,m_peakLoads=0;
    mutable std::mutex m_mutex;std::condition_variable m_changed;
};
} // namespace introvert::detection
