#include "intro_consistency.h"
#include "candidates.h"
#include <QCryptographicHash>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace introvert::detection {
namespace {
constexpr double durationSpread=1., alignmentTolerance=.5, edgeTolerance=.5;
struct Piece {
    QString peer,id;
    TimeRange range;
    double origin,minimum,mean;
    std::vector<TimeRange> fragments;
};
}

void checkIntroConsistency(const std::vector<AudioRecurrence> &matches,
    std::vector<AudioSegment> &selected,const Cancellation &cancel,qint64 &work,QVariantList *diagnostics)
{
    const auto tick=[&](qint64 amount=1) {
        work+=amount;
        if(work>audioSelectionWorkLimit)throw DetectionError("selection_limit_exceeded","Audio candidate selection exceeded its work budget.");
        checkCancelled(cancel);
    };
    std::vector<const AudioSegment *> intros;
    for(const auto &s:selected)if(s.type==SegmentType::Intro)intros.push_back(&s);
    if(intros.size()<5)return;
    std::sort(intros.begin(),intros.end(),[](const auto a,const auto b) {
        return std::make_pair(a->range.duration(),a->episodeId)<std::make_pair(b->range.duration(),b->episodeId);
    });
    size_t bestBegin=0,bestEnd=0;
    for(size_t begin=0,end=0;begin<intros.size();++begin) {
        while(end<intros.size() && intros[end]->range.duration()-intros[begin]->range.duration()<=durationSpread)++end;
        if(end-begin>bestEnd-bestBegin) {bestBegin=begin;bestEnd=end;}
    }
    const size_t cohortCount=bestEnd-bestBegin;
    if(cohortCount<4 || cohortCount*4<intros.size()*3)return;
    const double lower=intros[bestBegin+(cohortCount-1)/2]->range.duration();
    const double typical=lower+(intros[bestBegin+cohortCount/2]->range.duration()-lower)/2.;
    std::map<QString,TimeRange> references;
    for(size_t n=bestBegin;n<bestEnd;++n)references.emplace(intros[n]->episodeId,intros[n]->range);
    const auto layout=audioFingerprintLayout();const double hop=double(layout.hopSamples)/layout.sampleRate;
    std::vector<AudioSegment> checked;checked.reserve(selected.size());
    for(const auto &original:selected) {
        tick();
        if(original.type!=SegmentType::Intro || typical-original.range.duration()<=std::max(2.,typical*.1)) {
            checked.push_back(original);continue;
        }
        std::vector<Piece> pieces;
        for(const auto &m:matches) {
            tick();
            if(m.type!=SegmentType::Intro || (m.episodeA!=original.episodeId && m.episodeB!=original.episodeId))continue;
            const bool first=m.episodeA==original.episodeId;
            const auto peer=first?m.episodeB:m.episodeA;
            const auto ref=references.find(peer);if(ref==references.end())continue;
            const auto source=first?m.a:m.b,other=first?m.b:m.a;
            // Other recurring audio elsewhere in the peer is not intro evidence.
            if(other.start<ref->second.start-edgeTolerance || other.end>ref->second.end+edgeTolerance)continue;
            const auto fragments=m.evidence.value("fragments").toList();if(fragments.empty())continue;
            const auto refinement=m.evidence.value("refinement").toMap();
            const double rawStart=refinement.value(first?"raw_start_a":"raw_start_b",source.start).toDouble();
            if(!std::isfinite(rawStart))continue;
            const auto base=fragments.front().toMap().value("first_frame").toLongLong();
            Piece p{peer,m.diagnostic().value("id").toString(),source,source.start-(other.start-ref->second.start),m.minimumSimilarity,m.meanSimilarity,{}};
            if(!std::isfinite(p.origin) || base<0)continue;
            for(const auto &value:fragments) {
                tick();const auto fragment=value.toMap();
                const auto frame=fragment.value("first_frame").toLongLong(),count=fragment.value("frames").toLongLong();
                if(frame<base || count<=0 || double(frame)-base>m.frameCount || count>m.frameCount-(frame-base))continue;
                const double start=rawStart+double(frame-base)*hop,end=start+double(count)*hop;
                const double lo=std::max(source.start,start),hi=std::min(source.end,end);
                if(std::isfinite(lo) && std::isfinite(hi) && hi>lo)p.fragments.emplace_back(lo,hi);
            }
            if(!p.fragments.empty())pieces.push_back(std::move(p));
        }
        std::sort(pieces.begin(),pieces.end(),[](const auto &a,const auto &b) {return std::make_pair(a.origin,a.id)<std::make_pair(b.origin,b.id);});
        std::vector<AudioSegment> recovered;
        QSet<QString> testedGroups;
        for(const auto &seed:pieces) {
            tick();
            double start=seed.range.start,end=seed.range.end;
            std::vector<const Piece *> aligned;
            for(const auto &p:pieces) {
                tick();if(std::abs(p.origin-seed.origin)>alignmentTolerance)continue;
                aligned.push_back(&p);start=std::min(start,p.range.start);end=std::max(end,p.range.end);
            }
            const double length=end-start;
            if(std::abs(length-typical)>durationSpread || start>original.range.start || end<original.range.end)continue;
            QString groupKey;for(const auto p:aligned)groupKey+=p->id;
            if(testedGroups.contains(groupKey))continue;
            testedGroups.insert(groupKey);
            QSet<QString> starts,ends,peers;QStringList ids;
            std::vector<std::tuple<double,QString,int>> events;
            double minimum=1.,mean=0.;
            for(const auto p:aligned) {
                tick();peers.insert(p->peer);ids.append(p->id);minimum=std::min(minimum,p->minimum);mean+=p->mean;
                if(std::abs(p->range.start-start)<=edgeTolerance)starts.insert(p->peer);
                if(std::abs(p->range.end-end)<=edgeTolerance)ends.insert(p->peer);
                for(const auto &fragment:p->fragments) {
                    tick();events.emplace_back(fragment.start,p->peer,1);events.emplace_back(fragment.end,p->peer,-1);
                }
            }
            if(starts.size()<3 || ends.size()<3 || starts.intersect(ends).size()<2)continue;
            tick(qint64(events.size())*qint64(std::ceil(std::log2(std::max(size_t(2),events.size())))));
            std::sort(events.begin(),events.end());
            std::map<QString,int> active;int activePeers=0;
            double last=start,covered=0,gap=0,maxGap=0;
            for(const auto &event:events) {
                tick();const double time=std::get<0>(event),span=time-last;
                if(activePeers>=2) {covered+=span;if(span>0)gap=0;} else {gap+=span;maxGap=std::max(maxGap,gap);}
                auto &count=active[std::get<1>(event)];const bool wasActive=count>0;
                count+=std::get<2>(event);activePeers+=int(count>0)-int(wasActive);last=time;
            }
            maxGap=std::max(maxGap,gap+end-last);
            if(covered/length<.9 || maxGap>.75)continue;
            std::sort(ids.begin(),ids.end());ids.removeDuplicates();
            QVariantList sourceIds;for(const auto &id:ids)sourceIds.append(id);
            QStringList peerList=peers.values();std::sort(peerList.begin(),peerList.end());
            const QVariantMap evidence{{"method","cohort-fragments-v1"},{"cohort_count",int(cohortCount)},{"cohort_duration",typical},
                {"from_match_id",original.matchId},{"from_start",original.range.start},{"from_end",original.range.end},
                {"source_match_ids",sourceIds},{"consensus_coverage",covered/length},{"maximum_unsupported_gap",maxGap},
                {"boundary_peer_count",starts.size()}};
            const QVariantMap identity{{"method","cohort-fragments-v1"},{"episode",original.episodeId},{"start",start},{"end",end},{"source_match_ids",sourceIds}};
            const auto id=QString::fromLatin1(QCryptographicHash::hash(detectorJson(identity),QCryptographicHash::Sha256).toHex());
            recovered.push_back({original.episodeId,SegmentType::Intro,{start,end},peerList.front(),id,int(std::floor(covered/hop)),minimum,
                mean/aligned.size(),peerList,{{"selection_support",evidence}}});
        }
        bool ambiguous=false;
        if(!recovered.empty()) {
            double firstStart=recovered.front().range.start,lastStart=firstStart,firstEnd=recovered.front().range.end,lastEnd=firstEnd;
            for(const auto &s:recovered) {
                firstStart=std::min(firstStart,s.range.start);lastStart=std::max(lastStart,s.range.start);
                firstEnd=std::min(firstEnd,s.range.end);lastEnd=std::max(lastEnd,s.range.end);
            }
            ambiguous=lastStart-firstStart>edgeTolerance || lastEnd-firstEnd>edgeTolerance;
        }
        const bool resolved=!recovered.empty() && !ambiguous;
        if(resolved) {
            const auto best=std::max_element(recovered.begin(),recovered.end(),[](const auto &a,const auto &b) {
                return std::make_tuple(a.supportingPeers.size(),a.frameCount,a.meanSimilarity,a.matchId)<std::make_tuple(b.supportingPeers.size(),b.frameCount,b.meanSimilarity,b.matchId);
            });
            checked.push_back(*best);
        }
        if(diagnostics)for(auto &value:*diagnostics) {
            auto row=value.toMap();
            if(row.value("episode").toString()!=original.episodeId || row.value("segment_type").toString()!=QStringLiteral("intro"))continue;
            row["status"]=resolved?"selected":"inconsistent_duration";
            row["support_count"]=resolved?checked.back().supportingPeers.size():0;
            row["consistency"]=QVariantMap{{"method","cohort-fragments-v1"},{"cohort_count",int(cohortCount)},{"cohort_duration",typical},
                {"original_duration",original.range.duration()},{"recovered",resolved},{"ambiguous",ambiguous}};
            row["work"]=work;value=row;
        }
    }
    selected=std::move(checked);
}
} // namespace introvert::detection
