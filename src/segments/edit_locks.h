#pragma once
#include "identity.h"
#include <QMap>
#include <condition_variable>
#include <memory>
#include <mutex>

namespace introvert::segments {
// Shared by UI writers and detection jobs. A scan reserves its season before
// queueing; already-started writes drain before the scan snapshots revisions.
class EditLocks final {
    struct Entry {bool detecting=false;int writers=0;};
    struct State {std::mutex mutex;std::condition_variable changed;QMap<QString,Entry> entries;};
    std::shared_ptr<State> m_state=std::make_shared<State>();
    static QString route(providers::ProviderId provider,const QString &id) {
        return "provider:"+providers::providerName(provider)+':'+QString::number(id.size())+':'+id+"|season:";
    }
    static QStringList keys(const providers::ShowSummary &show,qint64 season) {
        QStringList result{route(show.identity.provider,show.identity.itemId)+QString::number(season)};
        for(const auto &key:showKeys(show))result.append("canonical:"+QString::number(key.size())+':'+key+"|season:"+QString::number(season));
        return result;
    }
public:
    class Lease final {
        friend class EditLocks;
        std::shared_ptr<State> m_state;QStringList m_keys;bool m_detection,m_armed=false;
        Lease(std::shared_ptr<State> state,QStringList keys,bool detection) : m_state(std::move(state)),m_keys(std::move(keys)),m_detection(detection) {}
    public:
        ~Lease() {
            if(!m_armed)return;
            std::lock_guard<std::mutex> lock(m_state->mutex);
            for(const auto &key:m_keys) {
                auto found=m_state->entries.find(key);
                if(m_detection)found->detecting=false;else --found->writers;
                if(!found->detecting && !found->writers)m_state->entries.erase(found);
            }
            m_state->changed.notify_all();
        }
        Lease(const Lease &)=delete;Lease &operator=(const Lease &)=delete;
        void waitForWriters(const providers::Cancellation &cancel) const {
            std::unique_lock<std::mutex> lock(m_state->mutex);
            for(;;) {
                providers::checkCancelled(cancel);bool waiting=false;
                for(const auto &key:m_keys)waiting|=m_state->entries.value(key).writers>0;
                if(!waiting)return;
                m_state->changed.wait_for(lock,std::chrono::milliseconds(20));
            }
        }
    };
    std::shared_ptr<Lease> detect(const providers::ShowSummary &show,qint64 season) {
        const auto names=keys(show,season);std::lock_guard<std::mutex> lock(m_state->mutex);
        for(const auto &key:names)if(m_state->entries.value(key).detecting)return {};
        for(const auto &key:names)m_state->entries[key];
        auto lease=std::shared_ptr<Lease>(new Lease(m_state,names,true));
        for(const auto &key:names)m_state->entries[key].detecting=true;
        lease->m_armed=true;
        return lease;
    }
    std::shared_ptr<Lease> edit(const QList<QPair<providers::ShowSummary,qint64>> &seasons) {
        QStringList names;for(const auto &season:seasons)names.append(keys(season.first,season.second));names.removeDuplicates();
        std::lock_guard<std::mutex> lock(m_state->mutex);
        for(const auto &key:names)if(m_state->entries.value(key).detecting)return {};
        for(const auto &key:names)m_state->entries[key];
        auto lease=std::shared_ptr<Lease>(new Lease(m_state,names,false));
        for(const auto &key:names)++m_state->entries[key].writers;
        lease->m_armed=true;
        return lease;
    }
    bool blocked(const providers::ShowSummary &show,qint64 season) const {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        for(const auto &key:keys(show,season))if(m_state->entries.value(key).detecting)return true;
        return false;
    }
    bool blocked(providers::ProviderId provider,const QString &show,std::optional<qint64> season) const {
        std::lock_guard<std::mutex> lock(m_state->mutex);const auto prefix=route(provider,show);
        if(season)return m_state->entries.value(prefix+QString::number(*season)).detecting;
        for(auto i=m_state->entries.cbegin();i!=m_state->entries.cend();++i)if(i.key().startsWith(prefix) && i->detecting)return true;
        return false;
    }
};
} // namespace introvert::segments
