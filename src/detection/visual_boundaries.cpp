#include "visual_boundaries.h"
#include <QSet>
#include <algorithm>
#include <array>
#include <cmath>

namespace introvert::detection {
namespace {
constexpr double radius=3.,step=.125;
using Frame=std::array<double,boundaryFeatureColumns>;
struct Track { QList<Frame> frames; QList<double> times; QList<int> cuts; bool limited=false; };
double distance(const Frame &a,const Frame &b) {
    double sum=0;for(size_t i=0;i<a.size();++i)sum+=std::abs(a[i]-b[i]);return sum/a.size();
}
bool black(const Frame &a) {double sum=0;for(auto v:a)sum+=v;return sum/a.size()<.04;}
Track track(const QVariantMap &edge,double reference)
{
    Track result; const auto observations=edge.value("observations").toList();
    if(observations.size()>64)throw DetectionError("visual_observation_limit","Too many boundary frames.");
    if(!observations.empty()) {
        const double first=edge.value("window_start").toDouble(),last=edge.value("window_end").toDouble();
        if(!std::isfinite(first) || !std::isfinite(last) || first<0 || first>reference || last<reference
            || last<=first || last-first>2*radius+1e-6 || observations.size()>qint64(std::ceil((last-first)/step-1e-8))
            || std::abs(observations.size()*step-(last-first))>step+.05
            || std::abs(observations.first().toMap().value("time").toDouble()-first)>1e-6)
            throw DetectionError("visual_invalid_observations","Incomplete visual boundary coverage.");
    }
    double previous=-1.;
    for(const auto &entry:observations) {
        const auto o=entry.toMap();const double t=o.value("time").toDouble();const auto values=o.value("values").toList();
        if(!std::isfinite(t) || t<0 || t<=previous || std::abs(t-reference)>radius+1e-6
            || (previous>=0 && std::abs(t-previous-step)>1e-5) || values.size()!=boundaryFeatureColumns)
            throw DetectionError("visual_invalid_observations","Invalid visual boundary coverage.");
        Frame f{};for(int i=0;i<values.size();++i) {
            const double v=values[i].toDouble();if(!std::isfinite(v) || v<0 || v>1)
                throw DetectionError("visual_invalid_observations","Invalid visual boundary feature.");
            f[size_t(i)]=v;
        }
        result.frames.append(f);result.times.append(t);previous=t;
    }
    // Four samples on each side keep cuts at a clipped window edge inconclusive.
    for(int i=4;i+3<result.frames.size();++i)
        if(distance(result.frames[i-1],result.frames[i])>=.025)result.cuts.append(i);
    // Never discard unchecked alternatives and claim a unique transition.
    result.limited=result.cuts.size()>8;
    return result;
}
bool supported(const Track &a,int i,const Track &b,int j,bool start,qint64 &work,const Cancellation &cancel)
{
    double inside=0,outside=0;bool darkOutside=true;int informative=0;
    for(int n=0;n<4;++n) {
        checkCancelled(cancel);if((work+=boundaryFeatureColumns*4)>8000000)return false;
        const int ai=start?i+n:i-1-n,bi=start?j+n:j-1-n;
        const int ao=start?i-1-n:i+n,bo=start?j-1-n:j+n;
        inside+=distance(a.frames[ai],b.frames[bi]);outside+=distance(a.frames[ao],b.frames[bo]);
        darkOutside=darkOutside && black(a.frames[ao]) && black(b.frames[bo]);
        informative+=!black(a.frames[ai]) && !black(b.frames[bi]);
    }
    // A matching shot alone cannot distinguish an internal intro cut. Require
    // recurrence to begin/end here, or a common black exterior on both episodes.
    return informative>=3 && inside/4<=.06 && (outside/4>=inside/4+.035 || darkOutside)
        && distance(a.frames[i-1],a.frames[i])>=.025 && distance(b.frames[j-1],b.frames[j])>=.025;
}
}
QVariantMap visualBoundaryDescription()
{
    return {{"version","recurring-visual-boundaries-v1"},{"available",true},{"width",160},{"height",96},
        {"columns",boundaryFeatureColumns},{"frames_per_second",8.},{"edge_radius_seconds",radius}};
}
QVariantList observeBoundaryFrame(const QByteArray &rgb,int width,int height,const Cancellation &cancel)
{
    if(width<8 || height<4 || width>1280 || height>720 || rgb.size()!=qint64(width)*height*3)
        throw DetectionError("visual_invalid_frame","Invalid visual boundary frame.");
    QVariantList result;result.reserve(boundaryFeatureColumns);
    for(int row=0;row<4;++row)for(int col=0;col<8;++col) {
        checkCancelled(cancel);std::array<double,3> sum{};qint64 count=0;
        for(int y=row*height/4;y<(row+1)*height/4;++y)for(int x=col*width/8;x<(col+1)*width/8;++x) {
            const auto at=(y*width+x)*3;for(int c=0;c<3;++c)sum[size_t(c)]+=uchar(rgb[at+c]);++count;
        }
        for(auto v:sum)result.append(v/(255.*count));
    }
    return result;
}
QVariantMap refineIntroVisualBoundaries(const QVariantList &audio,const QVariantMap &visual,const Cancellation &cancel)
{
    QVariantMap intros;QMap<QString,QMap<QString,Track>> tracks;
    for(const auto &entry:audio) {
        checkCancelled(cancel);const auto s=entry.toMap();if(s.value("segment_type")!="intro")continue;
        const auto id=s.value("episode").toString();
        if(intros.contains(id))throw DetectionError("visual_invalid_selection","Duplicate intro selection.");
        const double start=s.value("start").toDouble(),end=s.value("end").toDouble();
        if(!std::isfinite(start) || !std::isfinite(end) || start<0 || start>=end)
            throw DetectionError("visual_invalid_selection","Invalid intro selection.");
        intros[id]=s;
        const auto edges=visual.value(id).toMap().value("intro").toMap();
        for(const auto &edge:QStringList{"start","end"})tracks[id][edge]=track(edges.value(edge).toMap(),s.value(edge).toDouble());
    }
    qint64 work=0;QVariantList selected,decisions;
    for(const auto &entry:audio) {
        checkCancelled(cancel);auto s=entry.toMap();if(s.value("segment_type")!="intro") {selected.append(s);continue;}
        const auto id=s.value("episode").toString();QVariantMap sources{{"start","audio"},{"end","audio"}},evidence;
        QSet<QString> peers;for(const auto &peer:s.value("supporting_episodes").toList())peers.insert(peer.toString());
        peers.insert(s.value("peer").toString());peers.remove(id);peers.remove(QString());
        QStringList orderedPeers=peers.values();orderedPeers.sort();
        for(const auto &edge:QStringList{"start","end"}) {
            const auto &a=tracks[id][edge];QVariantList candidates;
            if(!a.limited)for(const int i:a.cuts) {
                if(work>8000000)break;
                QSet<QString> votes;int checked=0;
                for(const auto &peer:orderedPeers) {
                    if(work>8000000)break;
                    if(!intros.contains(peer))continue;
                    if(++checked>8)break;
                    const auto &b=tracks[peer][edge];if(b.limited)continue;
                    for(const int j:b.cuts)if(supported(a,i,b,j,edge=="start",work,cancel)) {votes.insert(peer);break;}
                }
                if(!votes.empty()) {QStringList ordered=votes.values();ordered.sort();candidates.append(QVariantMap{{"time",a.times[i]},
                    {"bracket_start",a.times[i-1]},{"bracket_end",a.times[i]},{"peers",ordered},
                    {"change",distance(a.frames[i-1],a.frames[i])}});}
            }
            // Consecutive samples of the same fade are one transition; distinct
            // alternatives remain ambiguous rather than snapping to the nearest cut.
            QVariantList groups;
            double previousCandidate=-1.;
            for(const auto &candidate:candidates) {
                const auto c=candidate.toMap();
                if(!groups.empty() && c.value("time").toDouble()-previousCandidate<=.251) {
                    if(c.value("change").toDouble()>groups.last().toMap().value("change").toDouble())groups.last()=c;
                } else groups.append(c);
                previousCandidate=c.value("time").toDouble();
            }
            QVariantMap decision{{"episode",id},{"segment_type","intro"},{"boundary",edge},{"status","audio_fallback"},
                {"reason",work>8000000?"visual_boundary_work_limit":a.limited?"visual_candidate_limit":groups.size()>1?"ambiguous_visual_transition":"no_supported_visual_transition"}};
            if(groups.size()==1 && !a.limited && work<=8000000) {
                const auto c=groups.first().toMap();const double t=c.value("time").toDouble();
                if((edge=="start" && t<s.value("end").toDouble()) || (edge=="end" && t>s.value("start").toDouble())) {
                    s["audio_selected_"+edge]=s.value(edge);s[edge]=t;sources[edge]="visual_recurrence";evidence[edge]=c;
                    decision["status"]="audio_visual";decision.remove("reason");decision["visual_time"]=t;
                }
            }
            decisions.append(decision);
        }
        s["boundary_sources"]=sources;s["visual_boundary_evidence"]=evidence;selected.append(s);
    }
    return {{"selected",selected},{"decisions",decisions},{"rule","audio-led-visual-edges-v2"},{"work",work},{"work_limit_reached",work>8000000}};
}
}
