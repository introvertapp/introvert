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
        const auto localEdges=original.evidence.value("refinement").toMap();
        bool variant=!resolved && localEdges.value("start_resolved").toBool() && localEdges.value("end_resolved").toBool();
        if(!resolved)for(const auto peer:intros) {
            if(peer->episodeId==original.episodeId || references.count(peer->episodeId)
                || std::abs(peer->range.duration()-original.range.duration())>durationSpread)continue;
            for(const auto &m:matches) {
                tick();if(m.type!=SegmentType::Intro)continue;
                const bool first=m.episodeA==original.episodeId && m.episodeB==peer->episodeId;
                if(!first && !(m.episodeB==original.episodeId && m.episodeA==peer->episodeId))continue;
                const auto source=first?m.a:m.b,other=first?m.b:m.a;
                const double a=std::min(source.end,original.range.end)-std::max(source.start,original.range.start);
                const double b=std::min(other.end,peer->range.end)-std::max(other.start,peer->range.start);
                if(a>=original.range.duration()*.9 && b>=peer->range.duration()*.9)variant=true;
            }
        }
        if(resolved) {
            const auto best=std::max_element(recovered.begin(),recovered.end(),[](const auto &a,const auto &b) {
                return std::make_tuple(a.supportingPeers.size(),a.frameCount,a.meanSimilarity,a.matchId)<std::make_tuple(b.supportingPeers.size(),b.frameCount,b.meanSimilarity,b.matchId);
            });
            checked.push_back(*best);
        } else if(variant)checked.push_back(original);
        if(diagnostics)for(auto &value:*diagnostics) {
            auto row=value.toMap();
            if(row.value("episode").toString()!=original.episodeId || row.value("segment_type").toString()!=QStringLiteral("intro"))continue;
            row["status"]=resolved || variant?"selected":"inconsistent_duration";
            row["support_count"]=resolved || variant?checked.back().supportingPeers.size():0;
            row["consistency"]=QVariantMap{{"method","cohort-fragments-v1"},{"cohort_count",int(cohortCount)},{"cohort_duration",typical},
                {"original_duration",original.range.duration()},{"recovered",resolved},{"ambiguous",ambiguous},{"variant_preserved",variant}};
            row["work"]=work;value=row;
        }
    }
    selected=std::move(checked);
}
void checkSeasonConsistency(const std::vector<AudioRecurrence> &matches,
    std::vector<AudioSegment> &selected,const Cancellation &cancel,qint64 &work,QVariantList *diagnostics,const QVariantList &windows,
    QVariantList *confirmationCandidates,const std::vector<AudioSegment> *priorSelections,bool finalReview)
{
    const auto tick=[&](qint64 amount=1) {
        work+=amount;
        if(work>audioSelectionWorkLimit)throw DetectionError("selection_limit_exceeded","Audio candidate selection exceeded its work budget.");
        checkCancelled(cancel);
    };
    const auto weightedMedian=[](std::vector<std::pair<double,double>> values) {
        std::sort(values.begin(),values.end());double weight=0,at=0;
        for(const auto &v:values)weight+=v.second;
        for(const auto &v:values) {at+=v.second;if(at>=weight*.5)return v.first;}
        return values.back().first;
    };
    // Snapshot references once. Recovered episodes never become references in
    // the same pass, so an inference cannot amplify itself across a season.
    const auto references=selected;
    std::map<std::pair<QString,SegmentType>,TimeRange> scopes;
    for(const auto &entry:windows) {
        const auto w=entry.toMap();const auto kind=w.value("segment_type").toString(),episode=w.value("episode").toString();
        bool a=false,b=false;const double start=w.value("start").toDouble(&a),end=w.value("end").toDouble(&b);
        if(episode.isEmpty() || (kind!="intro" && kind!="credits") || !a || !b)
            throw std::invalid_argument("Invalid season alignment window.");
        if(!scopes.emplace(std::make_pair(episode,kind=="intro"?SegmentType::Intro:SegmentType::Credits),TimeRange(start,end)).second)
            throw std::invalid_argument("Duplicate season alignment window.");
    }
    const auto layout=audioFingerprintLayout();const double hop=double(layout.hopSamples)/layout.sampleRate;
    std::map<std::pair<QString,SegmentType>,std::vector<const AudioRecurrence *>> indexed;
    for(const auto &m:matches) {
        tick();indexed[{m.episodeA,m.type}].push_back(&m);indexed[{m.episodeB,m.type}].push_back(&m);
    }
    for(const auto type:{SegmentType::Intro,SegmentType::Credits}) {
        std::vector<const AudioSegment *> remaining;
        for(const auto &s:references)if(s.type==type)remaining.push_back(&s);
        if(remaining.size()<size_t(finalReview?3:5))continue;
        const size_t referenceCount=remaining.size();
        const size_t episodeCount=std::max({referenceCount,
            size_t(std::count_if(indexed.begin(),indexed.end(),[&](const auto &entry){return entry.first.second==type;})),
            size_t(std::count_if(scopes.begin(),scopes.end(),[&](const auto &entry){return entry.first.second==type;}))});
        std::sort(remaining.begin(),remaining.end(),[](const auto a,const auto b) {
            return std::make_pair(a->range.duration(),a->episodeId)<std::make_pair(b->range.duration(),b->episodeId);
        });
        std::vector<std::vector<const AudioSegment *>> cohorts;
        // Find dense duration groups, rather than demanding near-perfect
        // agreement before doing any season-wide comparison. Separate groups
        // may describe genuine alternate openings/credits.
        while(!remaining.empty()) {
            size_t bestBegin=0,bestEnd=0;
            for(size_t begin=0,end=0;begin<remaining.size();++begin) {
                const double spread=std::max(4.,remaining[begin]->range.duration()*.05);
                while(end<remaining.size() && remaining[end]->range.duration()-remaining[begin]->range.duration()<=spread) {tick();++end;}
                if(end-begin>bestEnd-bestBegin) {bestBegin=begin;bestEnd=end;}
            }
            if(bestEnd-bestBegin<3)break;
            cohorts.emplace_back(remaining.begin()+bestBegin,remaining.begin()+bestEnd);
            remaining.erase(remaining.begin()+bestBegin,remaining.begin()+bestEnd);
        }
        if(cohorts.empty() || cohorts.front().size()<size_t(finalReview?3:4))continue;
        for(const auto &group:indexed) {
            if(group.first.second!=type)continue;
            tick();const auto &episode=group.first.first;
            auto existing=std::find_if(selected.begin(),selected.end(),[&](const auto &s){return s.episodeId==episode && s.type==type;});
            const AudioSegment *original=existing!=selected.end()?&*existing:nullptr;
            if(!original && priorSelections) {
                const auto prior=std::find_if(priorSelections->begin(),priorSelections->end(),[&](const auto &s){return s.episodeId==episode && s.type==type;});
                if(prior!=priorSelections->end())original=&*prior;
            }
            if(existing!=selected.end() && existing->evidence.value("selection_support").toMap().value("method")=="cohort-fragments-v1")continue;
            bool conflictingRecovery=false;
            if(diagnostics)for(const auto &entry:*diagnostics) {
                const auto row=entry.toMap();
                if(row.value("episode")==episode && row.value("segment_type")==segmentName(type)
                    && row.value("consistency").toMap().value("ambiguous").toBool())conflictingRecovery=true;
            }
            if(conflictingRecovery)continue;
            const auto &dominant=cohorts.front();
            const double typicalDuration=dominant[dominant.size()/2]->range.duration();
            if(finalReview) {
                // Review a fixed final snapshot once, never feeding hypotheses
                // back as references. Complete selections and minority versions
                // do not need to repeat their already accepted boundary work.
                if(dominant.size()*2<episodeCount || !original)continue;
                if(existing!=selected.end() && original->range.duration()>=typicalDuration*.5)continue;
                if(confirmationCandidates && std::any_of(confirmationCandidates->cbegin(),confirmationCandidates->cend(),[&](const auto &v) {
                    const auto p=v.toMap();return p.value("episode")==episode && p.value("segment_type")==segmentName(type);
                }))continue;
            }
            const auto edges=original?original->evidence.value("refinement").toMap():QVariantMap{};
            bool minorityPair=false;QSet<QString> internalPeers;
            if(original)for(const auto match:group.second) {
                tick();const auto &m=*match;const bool first=m.episodeA==episode;
                const auto peer=first?m.episodeB:m.episodeA;
                const auto ref=std::find_if(references.begin(),references.end(),[&](const auto &s){return s.episodeId==peer && s.type==type;});
                if(ref==references.end())continue;
                const auto source=first?m.a:m.b,other=first?m.b:m.a;
                const bool local=std::abs(source.start-original->range.start)<=1. && std::abs(source.end-original->range.end)<=1.;
                if(local && std::find(dominant.begin(),dominant.end(),&*ref)!=dominant.end()
                    && other.start>=ref->range.start-1. && other.end<=ref->range.end+1.
                    && (other.start-ref->range.start>std::max(2.,ref->range.duration()*.1)
                        || ref->range.end-other.end>std::max(2.,ref->range.duration()*.1)))internalPeers.insert(peer);
                if(local && std::abs(ref->range.duration()-original->range.duration())<=1.
                    && std::abs(other.start-ref->range.start)<=1. && std::abs(other.end-ref->range.end)<=1.
                    && m.evidence.value("coverage").toDouble()>=.9)minorityPair=true;
            }
            // PCM resolves where two recordings stop agreeing, not whether the
            // sequence itself ends. Several full references can contradict that
            // shortcut without invalidating an independently matched short pair.
            const bool internalFragment=(dominant.size()*4>=episodeCount*3 && internalPeers.size()>=4)
                || (finalReview && dominant.size()*2>=episodeCount && internalPeers.size()>=3);
            bool variant=minorityPair || (!finalReview && edges.value("start_resolved").toBool() && edges.value("end_resolved").toBool() && !internalFragment);
            if(original && !variant && dominant.size()*100>=referenceCount*65
                && typicalDuration-original->range.duration()>std::max(2.,typicalDuration*.1)) {
                std::vector<AudioSegment> enclosures;
                for(const auto match:group.second) {
                    tick();const auto &m=*match;const bool first=m.episodeA==episode;
                    const auto peer=first?m.episodeB:m.episodeA;
                    const auto ref=std::find_if(references.begin(),references.end(),[&](const auto &s){return s.episodeId==peer && s.type==type;});
                    const auto source=first?m.a:m.b,other=first?m.b:m.a;
                    const auto id=m.diagnostic().value("id").toString();const auto scope=scopes.find(group.first);
                    // A complete measured match to the peer's selected sequence
                    // can establish a rare version. A shared internal phrase or
                    // an inferred/recovered peer cannot lend it full boundaries.
                    if(ref==references.end() || std::find(dominant.begin(),dominant.end(),&*ref)!=dominant.end()
                        || ref->matchId!=id || !source.contains(original->range)
                        || source.duration()-original->range.duration()<=std::max(2.,source.duration()*.1)
                        || std::abs(other.start-ref->range.start)>1e-6 || std::abs(other.end-ref->range.end)>1e-6
                        || m.meanSimilarity<.9 || m.evidence.value("coverage").toDouble()<.9
                        || (scope!=scopes.end() && !scope->second.contains(source)))continue;
                    enclosures.push_back({episode,type,source,peer,id,m.frameCount,m.minimumSimilarity,m.meanSimilarity,{peer},m.evidence});
                }
                std::sort(enclosures.begin(),enclosures.end(),[](const auto &a,const auto &b) {
                    return std::make_tuple(-a.meanSimilarity,-a.frameCount,a.matchId)<std::make_tuple(-b.meanSimilarity,-b.frameCount,b.matchId);
                });
                bool ambiguous=false;
                if(!enclosures.empty())for(const auto &s:enclosures) {
                    tick();if(std::abs(s.range.start-enclosures.front().range.start)>1. || std::abs(s.range.end-enclosures.front().range.end)>1.)ambiguous=true;
                }
                if(!enclosures.empty() && !ambiguous) {
                    auto recovered=enclosures.front();
                    const QVariantMap support{{"method","peer-selected-enclosure-v1"},{"from_start",original->range.start},
                        {"from_end",original->range.end},{"reference_peer",recovered.peer},{"source_match_id",recovered.matchId}};
                    recovered.evidence["selection_support"]=support;
                    if(existing==selected.end()) {selected.push_back(std::move(recovered));existing=selected.end()-1;}
                    else *existing=std::move(recovered);
                    original=&*existing;variant=true;
                    if(diagnostics)for(auto &entry:*diagnostics) {
                        auto row=entry.toMap();if(row.value("episode")!=episode || row.value("segment_type")!=segmentName(type))continue;
                        row["status"]="selected";row.remove("reason");row["support_count"]=1;row["minority_recovery"]=support;row["work"]=work;entry=row;
                    }
                }
            }
            struct Proposal {TimeRange range;double score;QStringList peers;QVariantMap evidence;int frames;double minimum,mean;};
            std::vector<Proposal> proposals;
            std::vector<Proposal> provisional;
            QVariantList evaluations;
            QVariantList durationChecks;
            bool durationOutlier=false;
            for(const auto &cohort:cohorts) {
                const double typical=cohort[cohort.size()/2]->range.duration();
                const bool shortOutlier=original && cohort.size()>=size_t(finalReview?3:4)
                    && (finalReview?cohort.size()*2>=episodeCount:cohort.size()*100>=referenceCount*65)
                    && typical-original->range.duration()>std::max(2.,typical*.1);
                if(shortOutlier)durationChecks.append(QVariantMap{{"cohort_count",int(cohort.size())},{"reference_count",int(referenceCount)},
                    {"cohort_duration",typical},{"original_duration",original->range.duration()},{"variant_preserved",variant}});
                if(shortOutlier && variant)continue;
                durationOutlier=durationOutlier || shortOutlier;
                std::map<QString,const AudioSegment *> peers;
                for(const auto ref:cohort)if(ref->episodeId!=episode)peers.emplace(ref->episodeId,ref);
                if(peers.size()<3)continue;
                struct Vote {QString peer,id;double start,end,weight,minimum,mean;std::vector<TimeRange> fragments;QStringList sources;};
                std::map<QString,std::vector<Vote>> byPeer;
                for(const auto m:group.second) {
                    tick();const bool first=m->episodeA==episode;
                    const auto peer=first?m->episodeB:m->episodeA;
                    const auto ref=peers.find(peer);if(ref==peers.end())continue;
                    const auto source=first?m->a:m->b,other=first?m->b:m->a;
                    // An alignment elsewhere in the peer cannot borrow the
                    // selected segment's semantic label.
                    if(other.start<ref->second->range.start-2. || other.end>ref->second->range.end+2.)continue;
                    const auto fragments=m->evidence.value("fragments").toList();if(fragments.empty())continue;
                    const auto refinement=m->evidence.value("refinement").toMap();
                    const double rawStart=refinement.value(first?"raw_start_a":"raw_start_b",source.start).toDouble();
                    const auto base=fragments.front().toMap().value("first_frame").toLongLong();
                    if(!std::isfinite(rawStart) || base<0)continue;
                    Vote v{peer,m->diagnostic().value("id").toString(),source.start-other.start+ref->second->range.start,
                        source.end-other.end+ref->second->range.end,0,m->minimumSimilarity,m->meanSimilarity,{},{}};
                    v.sources.append(v.id);
                    for(const auto &entry:fragments) {
                        tick();const auto f=entry.toMap();const auto frame=f.value("first_frame").toLongLong(),count=f.value("frames").toLongLong();
                        if(frame<base || count<=0 || double(frame)-base>m->frameCount || count>m->frameCount-(frame-base))continue;
                        const double start=std::max(source.start,rawStart+double(frame-base)*hop);
                        const double end=std::min(source.end,rawStart+double(frame-base+count)*hop);
                        if(end>start) {v.fragments.emplace_back(start,end);v.weight+=end-start;}
                    }
                    v.weight=std::sqrt(std::min(1.,v.weight/ref->second->range.duration()))*m->meanSimilarity;
                    if(v.weight>0 && std::isfinite(v.start) && std::isfinite(v.end))byPeer[peer].push_back(std::move(v));
                }
                std::vector<Vote> votes;
                for(auto &peer:byPeer) {
                    // A peer gets one vote, irrespective of duplicate matches.
                    // Its longest verified alignment anchors compatible pieces.
                    auto &pieces=peer.second;
                    std::sort(pieces.begin(),pieces.end(),[](const auto &a,const auto &b) {
                        return std::make_tuple(-a.weight,a.start,a.end,a.id)<std::make_tuple(-b.weight,b.start,b.end,b.id);
                    });
                    auto vote=pieces.front();
                    bool conflicting=false;
                    for(size_t n=1;n<pieces.size();++n) {
                        tick();const auto &p=pieces[n];
                        const double tolerance=finalReview?double(layout.contextSamples)/layout.sampleRate:1.;
                        if(p.weight>=vote.weight*.9 && (std::abs(p.start-vote.start)>tolerance || std::abs(p.end-vote.end)>tolerance))conflicting=true;
                        if(std::abs(p.start-vote.start)<=(finalReview?tolerance:.5) && std::abs(p.end-vote.end)<=(finalReview?tolerance:.5)) {
                            vote.fragments.insert(vote.fragments.end(),p.fragments.begin(),p.fragments.end());
                            vote.sources.append(p.sources);
                        }
                    }
                    if(!conflicting)votes.push_back(std::move(vote));
                }
                // Outlier validation above does not depend on usable votes or
                // successful recovery. Missing evidence cannot certify a fragment.
                if(votes.size()<3)continue;
                // Coarse fingerprint endpoints can differ by a complete context
                // while identifying the same sequence. Only a grossly short
                // selection against a dominant cohort uses this wider recovery
                // agreement; ordinary selections keep their one-second rule.
                const double agreementTolerance=shortOutlier?std::max(1.,double(layout.contextSamples)/layout.sampleRate):1.;
                std::vector<std::pair<double,double>> starts,ends;
                double totalWeight=0;
                for(const auto &v:votes) {starts.emplace_back(v.start,v.weight);ends.emplace_back(v.end,v.weight);totalWeight+=v.weight;}
                double start=weightedMedian(starts),end=weightedMedian(ends),weight=0,minimum=1.,mean=0.;
                QStringList supporters,ids;std::vector<std::tuple<double,QString,int>> events;
                double measuredStart=end,measuredEnd=start;
                bool coarseAgreement=false;
                for(const auto &v:votes) {
                    tick();if(std::abs(v.start-start)>agreementTolerance || std::abs(v.end-end)>agreementTolerance)continue;
                    coarseAgreement=coarseAgreement || std::abs(v.start-start)>1. || std::abs(v.end-end)>1.;
                    weight+=v.weight;supporters.append(v.peer);ids.append(v.sources);minimum=std::min(minimum,v.minimum);mean+=v.mean;
                    for(const auto &f:v.fragments) {
                        tick();const double lo=std::max(start,f.start),hi=std::min(end,f.end);if(hi<=lo)continue;
                        measuredStart=std::min(measuredStart,lo);measuredEnd=std::max(measuredEnd,hi);
                        events.emplace_back(lo,v.peer,1);events.emplace_back(hi,v.peer,-1);
                    }
                }
                if(supporters.size()<3 || weight<totalWeight*.65 || start<0 || end<=start || events.empty())continue;
                // A small unmatched edge can be inferred from repeated aligned
                // peers. It must stay in the independently supplied hard window;
                // a caller without scopes gets measured bounds only.
                const auto scope=scopes.find(group.first);
                if(scope==scopes.end()) {
                    if(measuredStart-start>2. || end-measuredEnd>2.)continue;
                    start=std::max(start,measuredStart);end=std::min(end,measuredEnd);
                } else if(!scope->second.contains({start,end}))continue;
                tick(qint64(events.size())*qint64(std::ceil(std::log2(std::max(size_t(2),events.size())))));
                std::sort(events.begin(),events.end());std::map<QString,int> active;
                int activePeers=0;double last=start,covered=0,gap=0,maxGap=0,firstCovered=end,lastCovered=start;
                double unionCovered=0,unionGap=0,maxUnionGap=0,firstUnion=end,lastUnion=start;
                for(const auto &event:events) {
                    tick();const double time=std::clamp(std::get<0>(event),start,end),span=time-last;
                    if(activePeers>=1 && span>0) {unionCovered+=span;firstUnion=std::min(firstUnion,last);lastUnion=time;unionGap=0;}
                    else if(span>0) {unionGap+=span;if(lastUnion>start && time<end)maxUnionGap=std::max(maxUnionGap,unionGap);}
                    if(activePeers>=2 && span>0) {covered+=span;firstCovered=std::min(firstCovered,last);lastCovered=time;gap=0;}
                    else if(span>0) {gap+=span;if(lastCovered>start && time<end)maxGap=std::max(maxGap,gap);}
                    auto &count=active[std::get<1>(event)];const bool was=count>0;count+=std::get<2>(event);activePeers+=int(count>0)-int(was);last=time;
                }
                const double coverage=covered/(end-start);
                const double edgeAllowance=std::max(2.,(end-start)*.05);
                const double gapAllowance=std::clamp((end-start)*.05,.75,5.);
                const bool complementary=supporters.size()>=4 && coverage>=.75 && unionCovered/(end-start)>=.9
                    && maxUnionGap<=.75 && firstUnion-start<=2. && end-lastUnion<=2.;
                const bool supported=(coverage>=.85 && maxGap<=gapAllowance && firstCovered-start<=edgeAllowance && end-lastCovered<=edgeAllowance) || complementary;
                evaluations.append(QVariantMap{{"cohort_count",int(cohort.size())},{"support_count",supporters.size()},
                    {"start",start},{"end",end},{"coverage",coverage},{"maximum_unsupported_gap",maxGap},
                    {"unsupported_prefix",firstCovered-start},{"unsupported_suffix",end-lastCovered},
                    {"maximum_gap_allowance",gapAllowance},{"supported",supported}});
                const bool confirmable=confirmationCandidates && original && type==SegmentType::Intro
                    && scope!=scopes.end() && coverage>=.75 && maxGap<=std::min(15.,(end-start)*.15)
                    && firstCovered-start<=edgeAllowance && end-lastCovered<=2.
                    && supporters.size()>=std::max(4,int((cohort.size()+1)/2))
                    && end-start-original->range.duration()>std::max(2.,(end-start)*.1)
                    && start<=original->range.start && end>=original->range.end-1.;
                const double context=double(layout.contextSamples)/layout.sampleRate;
                const bool anchoredRecovery=confirmationCandidates && original && shortOutlier && !variant && !coarseAgreement
                    && scope!=scopes.end() && cohort.size()*4>=episodeCount*3
                    && supporters.size()>=std::max(4,int((cohort.size()*3+3)/4))
                    && weight>=totalWeight*.85 && mean/supporters.size()>=.9 && covered>=context
                    && (firstCovered-start<=context || end-lastCovered<=context)
                    && start<=original->range.start+1. && end>=original->range.end-1.;
                // Investigation is cheaper than accepting an inferred interval.
                // Independent peers, alignment quality and season prevalence
                // contribute continuously instead of requiring a 75% season.
                const double investigationScore=weight/totalWeight*(mean/supporters.size())
                    *std::sqrt(double(cohort.size())/episodeCount)*(1.-1./supporters.size());
                const bool investigate=confirmationCandidates && original && !variant && (finalReview || !coarseAgreement)
                    && scope!=scopes.end() && supporters.size()>=(finalReview?3:4) && investigationScore>=.5
                    && end-start-original->range.duration()>std::max(2.,(end-start)*.1)
                    && covered>=context && (finalReview || maxGap<=gapAllowance)
                    && (firstCovered-start<=context || end-lastCovered<=context)
                    && start<=original->range.start+1. && end>=original->range.end-1.;
                auto evaluation=evaluations.back().toMap();
                evaluation["investigation_score"]=investigationScore;evaluation["investigation_eligible"]=investigate;
                evaluation["anchor_recovery_eligible"]=anchoredRecovery;evaluations.back()=evaluation;
                if(finalReview && (!investigate || cohort.size()*2<episodeCount))continue;
                if(!supported && !confirmable && !anchoredRecovery && !investigate)continue;
                if(original) {
                    const double overlap=std::min(end,original->range.end)-std::max(start,original->range.start);
                    if(overlap<=0 || overlap/std::min(end-start,original->range.duration())<.65)continue;
                    // A longer variant may contain the entire common theme.
                    // Agreement on that shared subsection cannot erase its
                    // separately selected additional material.
                    if(original->range.duration()-(end-start)>std::max(2.,original->range.duration()*.1))continue;
                }
                // More independently aligned episodes gradually outweigh an
                // isolated example. Match quality and verified coverage still
                // matter; cohort size alone cannot supply any missing audio.
                const double score=weight*coverage;
                std::sort(ids.begin(),ids.end());ids.removeDuplicates();
                QVariantList sourceIds;for(const auto &id:ids)sourceIds.append(id);
                const QVariantMap evidence{{"method","season-weighted-alignment-v1"},{"cohort_count",int(cohort.size())},
                    {"support_count",supporters.size()},{"weight",weight},{"agreement",weight/totalWeight},{"consensus_coverage",coverage},
                    {"maximum_unsupported_gap",maxGap},{"boundary_consensus_tolerance_seconds",agreementTolerance},
                    {"coarse_endpoint_agreement",coarseAgreement},
                    {"single_peer_union_coverage",unionCovered/(end-start)},{"maximum_unverified_gap",maxUnionGap},{"complementary_fragments",complementary},
                    {"maximum_gap_allowance",gapAllowance},{"weighting","sqrt-verified-fraction-times-similarity"},
                    {"inferred_start_seconds",std::max(0.,measuredStart-start)},{"inferred_end_seconds",std::max(0.,end-measuredEnd)},
                    {"from_start",original?QVariant(original->range.start):QVariant()},
                    {"from_end",original?QVariant(original->range.end):QVariant()},
                    {"anchor_recovery",finalReview || ((anchoredRecovery || investigate) && !confirmable)},
                    {"final_season_review",finalReview},
                    {"investigation_score",investigationScore},
                    {"isolated_confirmation",investigate && !anchoredRecovery && !confirmable},
                    {"source_match_ids",sourceIds}};
                (supported && !finalReview?proposals:provisional).push_back({{start,end},score,supporters,evidence,int(std::floor(covered/hop)),minimum,mean/supporters.size()});
            }
            if(diagnostics)for(auto &entry:*diagnostics) {
                auto row=entry.toMap();if(row.value("episode")!=episode || row.value("segment_type")!=segmentName(type))continue;
                row[finalReview?"final_season_checks":"season_consensus_checks"]=evaluations;
                row[finalReview?"final_duration_checks":"season_duration_checks"]=durationChecks;entry=row;
            }
            std::sort(proposals.begin(),proposals.end(),[](const auto &a,const auto &b) {
                return std::make_tuple(-a.score,a.range.start,a.range.end)<std::make_tuple(-b.score,b.range.start,b.range.end);
            });
            if(!proposals.empty()) {
                const auto &best=proposals.front();bool ambiguous=false;
                for(size_t n=1;n<proposals.size();++n) {
                    const auto &p=proposals[n];
                    if(p.score>=best.score*.8 && (std::abs(p.range.start-best.range.start)>1. || std::abs(p.range.end-best.range.end)>1.))ambiguous=true;
                }
                // Conflicting recoveries also go through outlier validation;
                // they cannot silently reinstate a rejected short sequence.
                if(ambiguous) {
                    if(!durationOutlier)continue;
                    proposals.clear();provisional.clear();
                }
            }
            if(proposals.empty()) {
                if(!original)continue;
                if(existing!=selected.end() && type==SegmentType::Intro && !edges.value("start_resolved").toBool()) {
                    // A nearly complete version can still lose a few opening
                    // fingerprint contexts. Use the aligned hypothesis only to
                    // locate a bounded independent video check, never as a
                    // replacement timestamp on its own.
                    std::vector<std::pair<int,double>> searches;
                    for(const auto &entry:evaluations) {
                        const auto e=entry.toMap();const double first=e.value("start").toDouble(),last=e.value("end").toDouble(),length=last-first;
                        const double shift=original->range.start-first;
                        if(e.value("support_count").toInt()>=std::max(4,(e.value("cohort_count").toInt()+1)/2)
                            && original->range.duration()>=length*.9 && original->range.duration()<=length*1.1
                            && std::abs(last-original->range.end)<=1. && shift>0 && shift<=6.)
                            searches.emplace_back(e.value("support_count").toInt(),first);
                    }
                    std::sort(searches.begin(),searches.end(),[](const auto &a,const auto &b){return std::make_pair(-a.first,a.second)<std::make_pair(-b.first,b.second);});
                    if(!searches.empty()) {
                        bool ambiguous=false;
                        for(const auto &s:searches)if(s.first>=searches.front().first*.8 && std::abs(s.second-searches.front().second)>1.)ambiguous=true;
                        if(!ambiguous)existing->evidence["boundary_search"]=QVariantMap{{"start",QVariantMap{{"reference",searches.front().second},{"method","aligned-start-search-v1"}}}};
                    }
                }
                if(variant)continue;
                bool incomplete=durationOutlier;
                for(const auto &entry:evaluations) {
                    const auto e=entry.toMap();const double first=e.value("start").toDouble(),last=e.value("end").toDouble(),length=last-first;
                    const double overlap=std::min(last,original->range.end)-std::max(first,original->range.start);
                    if(e.value("cohort_count").toInt()>=4 && e.value("support_count").toInt()>=4
                        && length-original->range.duration()>std::max(2.,length*.1)
                        && overlap>=original->range.duration()*.9)incomplete=true;
                }
                if(incomplete && !variant) {
                    // Reviewing an existing credits interval must not erase it
                    // merely because a longer hypothesis remains unconfirmed.
                    // The exceptional quarantine is a radically short Intro.
                    if(finalReview && existing!=selected.end() && provisional.empty()
                        && (type==SegmentType::Credits || original->range.duration()>=typicalDuration*.25))continue;
                    if(diagnostics)for(auto &entry:*diagnostics) {
                        auto row=entry.toMap();if(row.value("episode")!=episode || row.value("segment_type")!=segmentName(type))continue;
                        row["status"]="inconsistent_duration";row["reason"]="unresolved_partial_sequence";
                        row["rejected_start"]=original->range.start;row["rejected_end"]=original->range.end;row["work"]=work;entry=row;
                    }
                    if(confirmationCandidates && !provisional.empty()) {
                        std::sort(provisional.begin(),provisional.end(),[](const auto &a,const auto &b){return std::make_tuple(-a.score,a.range.start,a.range.end)<std::make_tuple(-b.score,b.range.start,b.range.end);});
                        const auto &candidate=provisional.front();bool ambiguous=false;
                        for(size_t n=1;n<provisional.size();++n)if(provisional[n].score>=candidate.score*.8
                            && (std::abs(provisional[n].range.start-candidate.range.start)>1. || std::abs(provisional[n].range.end-candidate.range.end)>1.))ambiguous=true;
                        if(!ambiguous) {
                            QVariantList peerIds;for(const auto &peer:candidate.peers)peerIds.append(peer);
                            AudioSegment pending{episode,type,candidate.range,candidate.peers.front(),{},candidate.frames,candidate.minimum,candidate.mean,candidate.peers,
                                {{"season_consensus",candidate.evidence},{"requires_visual_confirmation",true}}};
                            if(finalReview && existing!=selected.end()
                                && (type==SegmentType::Credits || original->range.duration()>=typicalDuration*.25))
                                pending.evidence["review_fallback"]=original->diagnostic();
                            if(candidate.evidence.value("anchor_recovery").toBool()) {
                                QStringList required=type==SegmentType::Intro?QStringList{"start","end"}:QStringList{"start"};
                                QVariantMap anchors;
                                if(type==SegmentType::Intro)for(const auto &edge:QStringList{"start","end"}) {
                                    const double local=edge=="start"?original->range.start:original->range.end;
                                    double &boundary=edge=="start"?pending.range.start:pending.range.end;
                                    if(edges.value(edge+"_resolved").toBool() && std::abs(local-boundary)<=1.) {
                                        boundary=local;required.removeAll(edge);
                                        anchors[edge]=QVariantMap{{"resolved",true},{"time",local},{"match_id",original->matchId}};
                                    }
                                }
                                if(required.empty()) {required.append("end");anchors.remove("end");}
                                pending.evidence["boundary_anchors"]=anchors;
                                pending.evidence["boundary_confirmation"]=QVariantMap{{"method","season-anchor-recovery-v1"},{"required",required},
                                    {"isolated",!finalReview && candidate.evidence.value("isolated_confirmation").toBool()},
                                    {"season_review",finalReview && candidate.evidence.value("coarse_endpoint_agreement").toBool()}};
                            }
                            const bool anchored=candidate.evidence.value("anchor_recovery").toBool();
                            QVariantMap identity{{"method",anchored?"season-anchor-recovery-v1":"black-confirmed-sequence-v1"},{"episode",episode},
                                {"start",pending.range.start},{"end",pending.range.end},{"peers",peerIds}};
                            if(anchored)identity["kind"]=segmentName(type);
                            pending.matchId=QString::fromLatin1(QCryptographicHash::hash(detectorJson(identity),QCryptographicHash::Sha256).toHex());
                            confirmationCandidates->append(pending.diagnostic());
                        }
                    }
                    if(existing!=selected.end())selected.erase(existing);
                }
                continue;
            }
            auto best=proposals.front();
            QVariantMap anchors;
            if(original) {
                const auto refinement=original->evidence.value("refinement").toMap();
                for(const auto &edge:QStringList{"start","end"}) {
                    const double local=edge=="start"?original->range.start:original->range.end;
                    double &boundary=edge=="start"?best.range.start:best.range.end;
                    // Season recovery identifies the sequence. A nearby edge
                    // already resolved from PCM has finer evidence than its
                    // projected fingerprint boundary and keeps its own value.
                    if(refinement.value(edge+"_resolved").toBool() && std::abs(local-boundary)<=1.) {
                        boundary=local;
                        anchors[edge]=QVariantMap{{"method","pcm-alignment-edges-v1"},{"time",local},{"resolved",true},
                            {"match_id",original->matchId}};
                    }
                }
            }
            if(best.range.start>=best.range.end)continue;
            best.evidence["boundary_anchors"]=anchors;
            QVariantList peerIds;for(const auto &peer:best.peers)peerIds.append(peer);
            const QVariantMap identity{{"method","season-weighted-alignment-v1"},{"episode",episode},{"kind",segmentName(type)},
                {"start",best.range.start},{"end",best.range.end},{"peers",peerIds}};
            const auto id=QString::fromLatin1(QCryptographicHash::hash(detectorJson(identity),QCryptographicHash::Sha256).toHex());
            AudioSegment s{episode,type,best.range,best.peers.front(),id,best.frames,best.minimum,best.mean,best.peers,
                {{"season_consensus",best.evidence},{"boundary_anchors",anchors}}};
            if(existing==selected.end())selected.push_back(std::move(s));else *existing=std::move(s);
            if(diagnostics) {
                bool found=false;
                for(auto &entry:*diagnostics) {
                    auto row=entry.toMap();if(row.value("episode")!=episode || row.value("segment_type")!=segmentName(type))continue;
                    row["status"]="selected";row["support_count"]=best.peers.size();row["season_consensus"]=best.evidence;row["work"]=work;entry=row;found=true;
                }
                if(!found)diagnostics->append(QVariantMap{{"episode",episode},{"segment_type",segmentName(type)},
                    {"status","selected"},{"support_count",best.peers.size()},{"season_consensus",best.evidence},{"work",work}});
            }
        }
    }
    std::sort(selected.begin(),selected.end(),[](const auto &a,const auto &b){return std::make_pair(a.episodeId,a.type)<std::make_pair(b.episodeId,b.type);});
}
} // namespace introvert::detection
