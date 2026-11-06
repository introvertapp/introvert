#include "boundary_refinement.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace introvert::detection {
namespace {
constexpr double radius=3.;
struct Frame {double time,low,mean,high,maximum,saturation,difference;};
double number(const QVariantMap &m,const char *key)
{
    bool valid=false;const double value=m.value(key).toDouble(&valid);
    if(!valid || !std::isfinite(value))throw DetectionError("visual_invalid_observations","Invalid boundary statistics.");
    return value;
}
bool black(const Frame &f) {return f.maximum<=40. && f.mean<=28. && f.saturation<=20.;}
double median(std::vector<double> values)
{
    std::sort(values.begin(),values.end());return values[values.size()/2];
}
}
QVariantMap boundaryRefinementDescription()
{
    return {{"version","audio-supported-transitions-v3"},{"available",true},{"feature_format","signalstats-yuv-v1"},
        {"audio_boundary_protection",true},
        {"confirmation_candidates",true},
        {"maximum_search_offset_seconds",6.},
        {"width",160},{"height",96},{"edge_radius_seconds",radius},{"maximum_frames",2048},
        {"maximum_scan_seconds",6.},{"timeout_seconds",30.},{"timestamp_basis","source-frame-pts"}};
}
QVariantMap refineBoundary(const QVariantMap &window,const QString &kind,const QString &edge,double reference,const Cancellation &cancel,const QString &constraint)
{
    if((kind!="intro" && kind!="credits") || (edge!="start" && edge!="end") || (kind=="credits" && edge=="end")
        || !std::isfinite(reference) || reference<0 || (!constraint.isEmpty() && constraint!="preceding_black"
            && constraint!="consensus_transition" && constraint!="isolated_consensus_transition" && constraint!="season_transition")
        || (constraint=="preceding_black" && (kind!="intro" || edge!="start")))throw DetectionError("visual_invalid_scope","Invalid audio boundary.");
    QVariantMap decision{{"status","audio_fallback"},{"reason","no_supported_transition"},{"audio_time",reference}};
    if(window.value("status")!="analyzed") {decision["reason"]=window.value("code",window.value("reason","visual_unavailable"));return decision;}
    const double first=number(window,"window_start"),last=number(window,"window_end");
    const auto observations=window.value("observations").toList();
    if(first<0 || first>reference || last<reference || last<=first || last-first>6.000001
        || reference-first>radius+.000001 || last-reference>radius+.000001 || observations.size()>2048)
        throw DetectionError("visual_invalid_observations","Boundary statistics exceed their audio scope.");
    std::vector<Frame> frames;frames.reserve(size_t(observations.size()));
    double previous=-1.;
    for(const auto &entry:observations) {
        checkCancelled(cancel);const auto m=entry.toMap();
        Frame f{number(m,"time"),number(m,"low"),number(m,"mean"),number(m,"high"),number(m,"maximum"),number(m,"saturation"),number(m,"difference")};
        if(f.time<first-1e-6 || f.time>=last+1e-6 || f.time<=previous || (previous>=0 && f.time-previous>.250001)
            || f.low<0 || f.mean<0 || f.mean>f.maximum || f.high<f.low || f.high>f.maximum || f.maximum>255.
            || f.saturation<0 || f.saturation>255. || f.difference<0 || f.difference>255.)
            throw DetectionError("visual_invalid_observations","Invalid boundary frame coverage.");
        frames.push_back(f);previous=f.time;
    }
    if(frames.size()<4 || frames.front().time-first>.250001 || last-frames.back().time>.250001) {
        decision["reason"]="incomplete_frame_coverage";return decision;
    }
    struct Candidate {double time,before;QString method;};std::vector<Candidate> candidates;
    const bool exitBlack=kind=="intro" && edge=="start";
    for(size_t i=1;i<frames.size();++i) {
        checkCancelled(cancel);const auto &f=frames[i],&before=frames[i-1];
        // A short black run is useful on the appropriate side of the edge. A
        // dark story shot with highlights is not a blank frame. Require context
        // on both sides; clipped runs and single-frame flashes do not establish it.
        if(black(before)!=black(f) && (exitBlack?black(before):black(f))) {
            size_t a=i-1,b=i;
            if(exitBlack) {while(a>0 && black(frames[a-1]))--a;while(b+1<frames.size() && !black(frames[b+1]) && frames[b+1].time-f.time<.12)++b;}
            else {while(a>0 && !black(frames[a-1]) && before.time-frames[a-1].time<.12)--a;while(b+1<frames.size() && black(frames[b+1]))++b;}
            const double blackDuration=exitBlack?f.time-frames[a].time:frames[b].time-f.time+(frames[b].time-frames[b-1].time);
            const double contentDuration=exitBlack?frames[b].time-f.time:before.time-frames[a].time;
            if(blackDuration>=.08-1e-6 && contentDuration>=.08-1e-6 && (exitBlack?(a>0 || first==0.):i>1)
                && (constraint!="preceding_black" || f.time<=reference))
                candidates.push_back({f.time,before.time,"black_transition"});
        }
        if(constraint=="preceding_black")continue;
        if(f.difference<6. || black(before) || black(f))continue;
        std::vector<double> motion,meansBefore,meansAfter,highBefore,highAfter,satBefore,satAfter;
        size_t left=i,right=i+1;
        while(left>0 && f.time-frames[left-1].time<=.3)--left;
        while(right<frames.size() && frames[right].time-f.time<=.3)++right;
        for(size_t j=left;j<right;++j) {
            const auto &other=frames[j];if(j==i)continue;
            motion.push_back(other.difference);
            if(other.time<f.time) {meansBefore.push_back(other.mean);highBefore.push_back(other.high);satBefore.push_back(other.saturation);}
            else {meansAfter.push_back(other.mean);highAfter.push_back(other.high);satAfter.push_back(other.saturation);}
        }
        if(meansBefore.size()<2 || meansAfter.size()<2 || frames.front().time>f.time-.08 || frames.back().time<f.time+.08)continue;
        // A sharp, isolated change must persist. Flashes return to the old
        // levels; ordinary motion and gradual fades spread their changes out.
        const bool persistent=std::abs(median(meansBefore)-median(meansAfter))>=4.
            || std::abs(median(highBefore)-median(highAfter))>=8. || std::abs(median(satBefore)-median(satAfter))>=8.;
        if(persistent && f.difference>=std::max(6.,4.*median(motion)))
            candidates.push_back({f.time,before.time,"scene_cut"});
    }
    std::sort(candidates.begin(),candidates.end(),[](const auto &a,const auto &b){return a.time<b.time;});
    std::vector<Candidate> distinct;
    for(const auto &candidate:candidates) {
        if(!distinct.empty() && candidate.time-distinct.back().time<=.12) {
            if(candidate.method=="black_transition")distinct.back()=candidate;
        } else distinct.push_back(candidate);
    }
    if(constraint=="consensus_transition" || constraint=="isolated_consensus_transition") {
        // The season has already aligned this endpoint within one second.
        // Nearby story cuts outside that agreement band cannot corroborate it.
        QVariantList times;for(const auto &c:distinct)if(times.size()<64)times.append(c.time);
        decision["scope_candidates"]=int(distinct.size());decision["transition_times"]=times;
        decision["confirmation_radius_seconds"]=1.;
        if(constraint=="isolated_consensus_transition" && distinct.size()>1) {
            decision["candidates"]=int(distinct.size());decision["reason"]="ambiguous_visual_transitions";return decision;
        }
        distinct.erase(std::remove_if(distinct.begin(),distinct.end(),[&](const auto &c){return std::abs(c.time-reference)>1.;}),distinct.end());
    }
    decision["candidates"]=int(distinct.size());
    if(distinct.size()!=1) {if(distinct.size()>1)decision["reason"]="ambiguous_visual_transitions";return decision;}
    const auto &candidate=distinct.front();
    decision["status"]="resolved";decision["reason"]="supported_local_transition";decision["method"]=candidate.method;
    decision["time"]=candidate.time;decision["bracket_start"]=candidate.before;decision["bracket_end"]=candidate.time;
    return decision;
}
QVariantMap fuseBoundaryRefinement(const QVariantList &audio,const QVariantMap &visual,const Cancellation &cancel)
{
    if(audio.size()>4096)throw DetectionError("visual_invalid_selection","Too many audio selections.");
    QVariantList selected,decisions;
    for(const auto &entry:audio) {
        checkCancelled(cancel);auto segment=entry.toMap();const auto id=segment.value("episode").toString(),kind=segment.value("segment_type").toString();
        const double start=number(segment,"start"),end=number(segment,"end");
        if(id.isEmpty() || (kind!="intro" && kind!="credits") || start<0 || start>=end)throw DetectionError("visual_invalid_selection","Invalid audio selection.");
        const bool pending=segment.value("requires_visual_confirmation").toBool();bool confirmed=!pending;
        const auto explicitEdges=segment.value("confirmation_boundaries").toStringList();
        const auto required=explicitEdges.empty()?QStringList{"start"}:explicitEdges;
        QStringList confirmedEdges;
        if(!explicitEdges.empty() && (!pending || required.size()>2 || required.count("start")>1 || required.count("end")>1
            || std::any_of(required.begin(),required.end(),[&](const auto &e){return e!="start" && e!="end";})
            || (kind=="credits" && required.contains("end"))))throw DetectionError("visual_invalid_selection","Invalid confirmation boundaries.");
        auto proposed=segment;QVariantMap sources{{"start","audio"},{"end","audio"}},evidence;const auto firstDecision=decisions.size();
        const auto observations=visual.value(id).toMap().value(kind).toMap();
        for(const auto &edge:kind=="intro"?QStringList{"start","end"}:QStringList{"start"}) {
            QVariantMap decision;
            try {
                if(segment.value("boundary_protection").toMap().value(edge).toBool())
                    decision={{"status","audio_fallback"},{"reason","corroborated_audio_boundary"},{"audio_time",segment.value(edge)}};
                else {
                    const double audioTime=segment.value(edge).toDouble();
                    const double reference=segment.value("boundary_search").toMap().value(edge).toMap().value("reference",audioTime).toDouble();
                    if(!std::isfinite(reference) || reference<0 || std::abs(reference-audioTime)>6.
                        || (reference!=audioTime && (kind!="intro" || edge!="start" || reference>audioTime)))
                        throw DetectionError("visual_invalid_scope","Invalid aligned boundary search.");
                    const auto constraint=reference!=audioTime?QString("preceding_black"):segment.value("boundary_constraints").toMap().value(edge).toString();
                    decision=refineBoundary(observations.value(edge).toMap(),kind,edge,reference,cancel,constraint);
                    decision["audio_time"]=audioTime;decision["search_reference"]=reference;
                }
            }
            catch(const Cancelled &){throw;}
            catch(const DetectionError &error) {decision={{"status","audio_fallback"},{"reason",error.code},{"audio_time",segment.value(edge)}};}
            decision["episode"]=id;decision["segment_type"]=kind;decision["boundary"]=edge;
            if(pending && required.contains(edge) && decision.value("status")=="resolved"
                && (!explicitEdges.empty() || (decision.value("method")=="black_transition" && decision.value("time").toDouble()<=start)))confirmedEdges.append(edge);
            evidence[edge]=decision;decisions.append(decision);
            if(decision.value("status")=="resolved") {proposed[edge]=decision.value("time");sources[edge]="local_transition";}
        }
        if(pending)confirmed=confirmedEdges.size()==required.size();
        if(proposed.value("start").toDouble()>=proposed.value("end").toDouble()) {
            confirmed=false;
            proposed=segment;sources={{"start","audio"},{"end","audio"}};
            for(auto i=firstDecision;i<decisions.size();++i) {
                auto decision=decisions[i].toMap();decision["status"]="audio_fallback";decision["reason"]="conflicting_boundary_adjustments";
                decisions[i]=decision;evidence[decision.value("boundary").toString()]=decision;
            }
        }
        proposed["audio_selected_start"]=start;proposed["audio_selected_end"]=end;
        if(pending)proposed["confirmation_status"]=confirmed?"confirmed":"unconfirmed";
        proposed["boundary_sources"]=sources;proposed["visual_evidence"]=evidence;selected.append(proposed);
    }
    return {{"selected",selected},{"decisions",decisions}};
}
}
