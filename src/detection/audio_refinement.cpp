#include "audio_refinement.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace introvert::detection {
AudioRecurrence refineAudioMatch(const AudioRecurrence &raw, const TimeRange &wa, const TimeRange &wb,
    const ScopedAudioReader &read, const Cancellation &cancel, AudioRefinementBudget budget)
{
    if(budget.comparisons<100000 || budget.comparisons>6000000 || budget.peaks<1 || budget.peaks>32)
        throw std::invalid_argument("Invalid audio refinement budget.");
    auto result = raw;
    QVariantMap detail{{"method", "pcm-alignment-edges-v1"}, {"status", "unresolved"},
        {"raw_start_a", raw.a.start}, {"raw_end_a", raw.a.end}, {"raw_start_b", raw.b.start}, {"raw_end_b", raw.b.end}};
    qint64 work = 0; constexpr int rate = 8000, frame = 240, hop = 80;
    try {
        if (!read || !wa.contains(raw.a) || !wb.contains(raw.b)) throw DetectionError("refinement_unavailable", "No scoped audio for refinement.");
        const auto readAt = [&](uint32_t source, const TimeRange &window, qint64 first, int count, bool shortAllowed = false) {
            checkCancelled(cancel);
            if (first < 0 || count <= 0 || count > 262144 || first + count > qint64(std::floor(window.duration() * rate + 1e-8)))
                throw DetectionError("refinement_window_limit", "Refinement exceeds its audio scope.");
            auto pcm = read(source, rate, first, uint32_t(count));
            if (pcm.size() > size_t(count) || (!shortAllowed && pcm.size() != size_t(count))) throw DetectionError("refinement_incomplete_audio", "Incomplete refinement audio.");
            return pcm;
        };
        const auto correlation = [&](const auto &a, int ia, const auto &b, int ib, int count, int stride, bool derivative = false) {
            double aa = 0, bb = 0, ab = 0, sa = 0, sb = 0; int n = 0;
            for (int i = derivative ? 1 : 0; i < count; i += stride) {
                if (++work > budget.comparisons) throw DetectionError("refinement_work_limit", "Audio refinement exceeded its work budget.");
                if (work % 1024 == 0) checkCancelled(cancel);
                const double x = double(a[size_t(ia + i)]) - (derivative && i ? a[size_t(ia+i-1)] : 0);
                const double y = double(b[size_t(ib + i)]) - (derivative && i ? b[size_t(ib+i-1)] : 0);
                aa += x*x; bb += y*y; ab += x*y; sa += x; sb += y; ++n;
            }
            aa -= sa*sa/n; bb -= sb*sb/n; ab -= sa*sb/n;
            // No correlation can be established from silence or DC alone.
            if (std::min(aa, bb) / n < (derivative ? 1. : std::pow(32768. * .001, 2))) return 0.;
            return std::min(1., std::abs(ab) / std::sqrt(aa*bb));
        };
        const double coarseOffset = raw.b.start - raw.a.start;
        const int lagRadius = int(std::ceil((double(audioFingerprintLayout().hopSamples) / audioFingerprintLayout().sampleRate + .02) * rate));
        const double center = (raw.a.start + raw.a.end) / 2;
        const double lower = std::max(wa.start, wb.start - coarseOffset + double(lagRadius) / rate);
        const double upper = std::min(wa.end, wb.end - coarseOffset - double(lagRadius) / rate);
        const int anchorCount = int(std::min(1., upper - lower) * rate);
        if (anchorCount < rate / 2) throw DetectionError("refinement_anchor_unavailable", "No complete alignment anchor.");
        const double anchorStart = std::clamp(center - double(anchorCount) / (2*rate), lower, upper - double(anchorCount)/rate);
        const qint64 firstA = qint64(std::ceil((anchorStart - wa.start) * rate));
        const qint64 firstB = qint64(std::llround((wa.start + double(firstA)/rate + coarseOffset - wb.start) * rate));
        const auto a = readAt(0, wa, firstA, anchorCount), b = readAt(1, wb, firstB - lagRadius, anchorCount + 2*lagRadius);
        int best = 0; double quality = -1.; std::vector<std::pair<double,int>> peaks;
        for (int lag = -lagRadius; lag <= lagRadius; lag += 4) {
            const double score = correlation(a, 0, b, lagRadius + lag, anchorCount, 4);
            peaks.emplace_back(score,lag);
        }
        std::sort(peaks.begin(),peaks.end(),[](const auto &x,const auto &y) { return x.first!=y.first ? x.first>y.first : x.second<y.second; });
        std::vector<int> searched;
        for (const auto &peak : peaks) {
            if (searched.size()==size_t(budget.peaks)) break;
            if (std::any_of(searched.begin(),searched.end(),[&](int lag) { return std::abs(lag-peak.second)<=8; })) continue;
            searched.push_back(peak.second);
            // A periodic coarse peak can beat the true sub-sample alignment.
            // Recheck independent peaks at the actual PCM sample resolution.
            for (int lag=std::max(-lagRadius,peak.second-3);lag<=std::min(lagRadius,peak.second+3);++lag) {
                const double score=correlation(a,0,b,lagRadius+lag,anchorCount,1);
                if(score>quality) {quality=score;best=lag;}
            }
        }
        detail["alignment_correlation"] = quality;
        if (quality < .65) throw DetectionError("refinement_alignment_weak", "The local audio alignment is inconclusive.");
        const double offset = wb.start + double(firstB + best)/rate - (wa.start + double(firstA)/rate);
        detail["offset_seconds"] = offset; detail["sample_rate"] = rate;
        const double radius = double(audioFingerprintLayout().contextSamples) / audioFingerprintLayout().sampleRate;
        const auto edge = [&](double target, bool start) -> std::optional<double> {
            const auto unresolved = [&](const QString &reason) -> std::optional<double> { detail[start ? "start_reason" : "end_reason"]=reason; return {}; };
            const double lo = std::max({wa.start, wb.start - offset, target - radius});
            const double hi = std::min({wa.end, wb.end - offset, target + radius});
            const qint64 ia = qint64(std::ceil((lo - wa.start) * rate));
            const double origin = wa.start + double(ia)/rate;
            const qint64 ib = qint64(std::llround((origin + offset - wb.start) * rate));
            int count = int(std::min({qint64(std::floor((hi-origin)*rate)), qint64(std::floor(wb.duration()*rate + 1e-8))-ib,
                qint64(std::floor(wa.duration()*rate + 1e-8))-ia}));
            if (count < frame * 4) return unresolved("insufficient_audio");
            const auto x = readAt(0, wa, ia, count, true), y = readAt(1, wb, ib, count, true);
            const bool availableEnd = std::min(x.size(),y.size()) < size_t(count);
            count = int(std::min(x.size(),y.size())); if (count < frame*4) return unresolved("insufficient_audio");
            struct Run { int first, last; double overlap; }; std::vector<Run> runs;
            int first = -1, steps = 0;
            const auto append = [&](int last) {
                if (first < 0 || last-first < 5) return;
                const double begin = origin + double(first*hop + frame/2)/rate, end = origin + double((last-1)*hop + frame/2)/rate;
                runs.push_back({first,last,std::max(0.,std::min(end,raw.a.end)-std::max(begin,raw.a.start))});
            };
            for (int p = 0; p + frame <= count; p += hop, ++steps) {
                // Recognition tolerates mixtures; a fine cut needs substantially
                // stronger local evidence, including changes beyond a common tone.
                if (correlation(x,p,y,p,frame,1) >= std::max(.8,.95*quality)
                    && correlation(x,p,y,p,frame,1,true) >= std::max(.75,.9*quality)) { if (first < 0) first = steps; }
                else { append(steps); first = -1; }
            }
            append(steps);
            if (runs.empty()) return unresolved("no_matching_audio");
            const auto selected = std::max_element(runs.begin(), runs.end(), [](const auto &u, const auto &v) { return u.overlap < v.overlap; });
            if (selected->overlap < .25) return unresolved("insufficient_edge_support");
            // Other sustained matches outside this edge make its location ambiguous.
            for (const auto &run : runs) if (&run != &*selected && run.last-run.first >= 20
                && (start ? run.first < selected->first : run.last > selected->last)) return unresolved("multiple_matched_runs");
            if (start && selected->first == 0) {
                if (std::min(origin-wa.start, origin+offset-wb.start) > .001) return unresolved("no_start_transition");
                return origin;
            }
            if (!start && selected->last == steps) {
                const double end = origin + double(count)/rate;
                if (!availableEnd && std::min(wa.end-end,wb.end-end-offset) > .001) return unresolved("no_end_transition");
                return end;
            }
            return origin + double((start ? selected->first : selected->last-1)*hop + frame/2)/rate;
        };
        std::optional<double> start,end;
        try { start=edge(raw.a.start,true); } catch(const Cancelled &) {throw;} catch(const DetectionError &error) {detail["start_reason"]=error.code;}
        try { end=edge(raw.a.end,false); } catch(const Cancelled &) {throw;} catch(const DetectionError &error) {detail["end_reason"]=error.code;}
        double sa = raw.a.start, ea = raw.a.end, sb = raw.b.start, eb = raw.b.end;
        if (start) { sa=std::clamp(*start,wa.start,wa.end); sb=std::clamp(sa+offset,wb.start,wb.end); }
        if (end) { ea=std::clamp(*end,wa.start,wa.end); eb=std::clamp(ea+offset,wb.start,wb.end); }
        if (!(sa<ea && sb<eb)) throw DetectionError("refinement_invalid_range", "The refined edges are inconsistent.");
        const TimeRange ra(sa,ea), rb(sb,eb);
        if (!wa.contains(ra) || !wb.contains(rb)) throw DetectionError("refinement_window_limit", "Refined boundaries exceed the hard window.");
        result.a=ra; result.b=rb; detail["start_resolved"]=bool(start); detail["end_resolved"]=bool(end);
        detail["status"] = start && end ? "refined" : start || end ? "partial" : "unresolved";
    } catch (const Cancelled &) { throw; }
    catch (const DetectionError &error) { detail["reason"] = error.code; }
    detail["work"] = work; detail["comparison_limit"]=budget.comparisons; detail["peak_limit"]=budget.peaks;
    result.evidence["refinement"] = detail; return result;
}
}
