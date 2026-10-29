#include "visual_credits.h"
#include <QSet>
#include <algorithm>
#include <cmath>

namespace introvert::detection {
namespace {
QVariantList features(const QVariant &observation) { return observation.toMap().value("values").toList(); }
double time(const QVariant &observation) { return observation.toMap().value("time").toDouble(); }
void validateObservations(const QVariantList &observations, const TimeRange &window)
{
    if (observations.size() > 3000) throw DetectionError("visual_observation_limit", "Too many visual observations.");
    double previous = -1.;
    for (const auto &o : observations) {
        const auto values = features(o); const auto t = time(o);
        if (!std::isfinite(t) || t < window.start || t >= window.end || t <= previous || values.size() != visualFeatureColumns)
            throw DetectionError("visual_invalid_observations", "Invalid visual feature coverage.");
        for (const auto &v : values) if (!std::isfinite(v.toDouble()) || v.toDouble() < 0)
            throw DetectionError("visual_invalid_observations", "Invalid visual feature value.");
        if (values[TextLines].toDouble()>512 || values[TextRows].toDouble()>values[TextLines].toDouble()
            || values[PairedRows].toDouble()>values[TextRows].toDouble() || values[RoleLines].toDouble()>values[TextLines].toDouble()
            || values[NameLines].toDouble()>values[TextLines].toDouble() || values[BottomRows].toDouble()>values[TextRows].toDouble()
            || values[TextConfidence].toDouble()>1 || values[TextArea].toDouble()>1 || values[TextHeight].toDouble()>1 || values[DarkFraction].toDouble()>1)
            throw DetectionError("visual_invalid_observations", "Visual feature values exceed their layout.");
        previous = t;
    }
}
}
QVariantList visualFrameFeatures(const QList<VisualTextLine> &input, double dark)
{
    auto lines = input; std::sort(lines.begin(), lines.end(), [](const auto &a, const auto &b) { return a.y + a.height/2 < b.y + b.height/2; });
    struct Row { double y, height; int count; }; QList<Row> rows;
    double area = 0., confidence = 0., top = 1., bottom = 0.; int roles = 0, names = 0;
    for (const auto &l : lines) {
        area += l.width*l.height; confidence += l.confidence; roles += l.role; names += l.name;
        top = std::min(top,l.y); bottom = std::max(bottom,l.y+l.height);
        if (!rows.empty() && std::abs(l.y+l.height/2-rows.last().y) <= .6*std::max(l.height,rows.last().height)) ++rows.last().count;
        else rows.append({l.y+l.height/2,l.height,1});
    }
    int paired = 0, low = 0; for (const auto &r : rows) { paired += r.count >= 2; low += r.y >= .78; }
    return {lines.size(), rows.size(), paired, roles, names, lines.empty() ? 0. : confidence/lines.size(),
        std::min(1.,area), std::max(0.,bottom-top), dark, low};
}
bool creditFrame(const QVariantList &v)
{
    if (v.size() != visualFeatureColumns) return false;
    const int rows = v[TextRows].toInt(), names = v[NameLines].toInt(), roles = v[RoleLines].toInt();
    // Ordinary one/two-line subtitles, station bugs and title cards cannot qualify.
    return rows >= 3 && v[TextConfidence].toDouble() >= .45 && v[TextArea].toDouble() >= .006 && v[TextHeight].toDouble() >= .08
        && ((rows >= 4 && names >= 3) || v[PairedRows].toInt() >= 3 || roles >= 2);
}
bool leadingCreditFrame(const QVariantList &v)
{
    if(v.size()!=visualFeatureColumns)return false;
    // A sparse role/name card is evidence only when attached to an independently
    // confirmed credit sequence. Bottom-only subtitles and lone titles fail.
    const int rows=v[TextRows].toInt();
    return rows>=2 && v[RoleLines].toInt()>=1 && v[NameLines].toInt()>=1
        && v[TextConfidence].toDouble()>=.45 && v[TextArea].toDouble()>=.003 && v[TextHeight].toDouble()>=.04
        && v[DarkFraction].toDouble()>=.8 && v[BottomRows].toInt()*2<rows;
}
QVariantMap findVisualCredits(const QVariantList &observations, double step, const TimeRange &window, const Cancellation &cancel)
{
    validateObservations(observations,window);
    if (!std::isfinite(step) || step < .1 || step > 2.) throw std::invalid_argument("Invalid visual cadence");
    QVariantList candidates; int first = -1, last = -1, count = 0, roles = 0, paired = 0;
    const auto finish = [&] {
        if (first < 0) return;
        const double start = time(observations[first]), end = std::min(window.end,time(observations[last])+step);
        const double coverage = count*step/(end-start);
        if (count >= 4 && end-start >= 3. && coverage >= .7)
            candidates.append(QVariantMap{{"start",start},{"end",end},{"frames",count},{"coverage",coverage},
                {"role_frames",roles},{"paired_frames",paired},{"independent_support",roles >= 2},{"start_resolved",false},
                {"method","temporal-credit-layout-v1"}});
        first = last = -1; count = roles = paired = 0;
    };
    for (int i=0;i<observations.size();++i) {
        checkCancelled(cancel);
        if (last >= 0 && time(observations[i])-time(observations[last]) > 2.01*step) finish();
        const auto values = features(observations[i]);
        if (!creditFrame(values)) continue;
        if (first < 0) first = i;
        last = i; ++count; roles += values[RoleLines].toInt() > 0; paired += values[PairedRows].toInt() >= 3;
    }
    finish();
    for(auto &entry:candidates) {
        auto c=entry.toMap();const double anchor=c.value("start").toDouble();
        int first=0;while(first<observations.size() && time(observations[first])<anchor-1e-8)++first;
        int lead=-1,frames=0;double next=anchor;
        for(int i=first-1;i>=0;--i) {
            const auto v=features(observations[i]);const auto t=time(observations[i]);
            if(next-t>1.01)break;
            if(leadingCreditFrame(v)) {lead=i;++frames;next=t;}
            else if(v[TextRows].toInt()==0 && v[DarkFraction].toDouble()>=.9)continue;
            else break;
        }
        if(lead>=0) {
            c["leading_card_frames"]=frames;c["anchor_start"]=anchor;
            if(frames*step>=1.-1e-8) {c["start"]=time(observations[lead]);c["leading_card_confirmed"]=true;c["method"]="confirmed-credit-sequence-v2";}
            else c["leading_card_inconclusive"]=true;
        }
        entry=c;
    }
    return {{"status",candidates.empty() ? "no_visual_candidate" : "candidates"},{"candidates",candidates},
        {"observations",observations.size()},{"cadence_seconds",step}};
}
QVariantList refineVisualCredits(const QVariantList &candidates, const QVariantList &observations, const TimeRange &window, const Cancellation &cancel)
{
    validateObservations(observations,window); QVariantList result;
    for (const auto &entry : candidates) {
        checkCancelled(cancel); auto c = entry.toMap(); const double coarse = c.value("start").toDouble();
        if(c.value("leading_card_inconclusive").toBool()){c["start_resolved"]=false;c["refinement_reason"]="earlier_credit_card_unresolved";result.append(c);continue;}
        const auto positive=[&](int i){const auto v=features(observations[i]);return creditFrame(v) || (c.value("leading_card_confirmed").toBool() && leadingCreditFrame(v));};
        double previous = -1.; bool negative = false, resolved = false;
        for (int i=0;i+1<observations.size();++i) {
            const double t = time(observations[i]); if (t < coarse-2.01 || t > coarse+1.01) continue;
            if (!positive(i)) { negative = true; previous = t; continue; }
            // Keep a quarter-second of consecutive support at every cadence.
            // Higher Effort adds observations, not a weaker temporal criterion.
            int last=i;bool supported=true;
            while(time(observations[last])-t<.249) {
                if(last+1>=observations.size() || time(observations[last+1])-time(observations[last])>.251
                    || !positive(last+1)) {supported=false;break;}
                ++last;
            }
            if(!supported)continue;
            if (negative && t-previous<=.251) {
                c["coarse_start"] = coarse; c["start"] = t; c["start_resolved"] = true;
                c["boundary_bracket_start"] = negative ? previous : t; c["boundary_bracket_end"] = t;
                c["boundary_method"] = "dense-credit-text-onset-v1"; resolved = true;
            }
            break;
        }
        if (!resolved) c["refinement_reason"] = "no_supported_text_transition";
        result.append(c);
    }
    return result;
}
QVariantMap fuseCreditCandidates(const QVariantList &audio, const QVariantList &matches, const QVariantMap &visual, const Cancellation &cancel,double edgeRadius)
{
    if(!std::isfinite(edgeRadius) || edgeRadius<=0 || edgeRadius>15.)throw std::invalid_argument("Invalid visual edge radius.");
    Q_UNUSED(matches);
    QVariantList selected, decisions;
    for (const auto &entry : audio) {
        checkCancelled(cancel); auto a=entry.toMap();
        if (a.value("segment_type")!="credits") { selected.append(a); continue; }
        const auto episode=a.value("episode").toString();
        const double start=a.value("start").toDouble(),end=a.value("end").toDouble();
        if(visual.value(episode).toMap().value("limited").toBool()) {
            // An unchecked competing candidate cannot be treated as negative
            // evidence merely because this Effort level reached its budget.
            selected.append(a);decisions.append(QVariantMap{{"episode",episode},{"status","audio_fallback"},{"reason","visual_refinement_candidate_limit"}});continue;
        }
        QVariantList qualified;
        for (const auto &entry : visual.value(episode).toMap().value("candidates").toList()) {
            const auto c=entry.toMap(); const double t=c.value("start").toDouble();
            // A resolved local transition refines only the existing audio start.
            // Never create a segment, change its measured end or recover farther away.
            if (c.value("start_resolved").toBool() && std::isfinite(t) && std::abs(t-start)<=edgeRadius+.000001 && t<end
                && std::min(end,c.value("end").toDouble())-std::max(start,t)>=std::min(3.,.25*(end-start))) qualified.append(c);
        }
        if (qualified.size()!=1) {
            selected.append(a);
            decisions.append(QVariantMap{{"episode",episode},{"status","audio_fallback"},
                {"reason",qualified.empty()?"no_resolved_local_visual_transition":"ambiguous_visual_transition"}});
            continue;
        }
        const auto v=qualified.first().toMap(); a["audio_selected_start"]=start; a["start"]=v.value("start");
        a["boundary_sources"]=QVariantMap{{"start","visual_credit_text"},{"end","audio"}}; a["visual_evidence"]=v;
        selected.append(a);
        decisions.append(QVariantMap{{"episode",episode},{"status","audio_visual"},{"visual_start",v.value("start")},
            {"audio_start",start},{"start_disagreement_seconds",std::abs(start-v.value("start").toDouble())}});
    }
    return {{"selected",selected},{"decisions",decisions},{"rule","audio-led-credits-edge-v1"}};
}
} // namespace introvert::detection
