#include "algorithm_api.h"
#include "algorithm_data.h"
#include "audio_recurrence.h"
#include "audio_refinement.h"
#include "visual_credits.h"
#include "algorithm_manifest.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <limits>

namespace introvert::detection {
namespace {
QVariantMap effortPolicy(const QString &name)
{
    if(!name.isEmpty() && name!="light" && name!="medium" && name!="high")throw std::invalid_argument("Invalid detector effort.");
    const bool light=name=="light",high=name=="high";
    return {{"version","effort-v1"},{"name",name.isEmpty()?"legacy":name},
        {"audio_candidates",light?2:high?16:8},{"audio_comparisons",light?2000000:high?6000000:3000000},
        {"audio_peaks",light?4:high?32:16},{"visual_radius",light||name.isEmpty()?5.:high?15.:10.},
        {"visual_fps",high?2.:1.},{"visual_dense_fps",high?8.:4.},{"visual_candidates",light?1:high?8:4}};
}
AudioRecurrenceParameters parameters(const QVariantMap &m)
{
    AudioRecurrenceParameters p;
    const auto integer = [&](const char *key, int &target) { if (m.contains(key)) { bool ok; const auto n = m.value(key).toLongLong(&ok); if (!ok || n < 0 || n > std::numeric_limits<int>::max()) throw std::invalid_argument("Invalid algorithm parameter"); target = int(n); } };
    integer("seed_bit_errors", p.seedBitErrors); integer("frame_bit_errors", p.frameBitErrors); integer("minimum_seeds", p.minimumSeeds);
    integer("minimum_distinct_points", p.minimumDistinctPoints); integer("maximum_seed_bucket_size", p.maximumSeedBucketSize);
    integer("maximum_matches", p.maximumMatches); integer("feed_samples", p.feedSamples);
    if (m.contains("minimum_seconds")) p.minimumSeconds = m.value("minimum_seconds").toDouble();
    if (m.contains("silence_floor_dbfs")) p.silenceFloorDbfs = m.value("silence_floor_dbfs").toDouble();
    if (m.contains("maximum_interruption_seconds")) p.maximumInterruptionSeconds = m.value("maximum_interruption_seconds").toDouble();
    if (m.contains("minimum_anchor_seconds")) p.minimumAnchorSeconds = m.value("minimum_anchor_seconds").toDouble();
    if (m.contains("minimum_coverage")) p.minimumCoverage = m.value("minimum_coverage").toDouble();
    if (m.contains("maximum_comparisons_per_pair")) p.maximumComparisons = m.value("maximum_comparisons_per_pair").toLongLong();
    if (m.contains("maximum_track_bytes")) p.maximumTrackBytes = m.value("maximum_track_bytes").toLongLong();
    p.validate(); return p;
}
const char *metadata()
{
    static const auto bytes = [] {
        auto m = QJsonDocument::fromJson(algorithmManifestJson).object(); m["qt_version"] = QT_VERSION_STR;
#ifdef INTROVERT_ALGORITHM_TEST_VERSION
        m["version"] = INTROVERT_ALGORITHM_TEST_VERSION;
#endif
        return QJsonDocument(m).toJson(QJsonDocument::Compact);
    }();
    return bytes.constData();
}
AudioRecurrence recurrence(const QVariantMap &m)
{
    const auto kind = m.value("segment_type").toString();
    if (kind != "intro" && kind != "credits") throw std::invalid_argument("Invalid recurrence kind");
    return {m.value("episode_a").toString(), m.value("episode_b").toString(),
        {m.value("start_a").toDouble(), m.value("end_a").toDouble()}, {m.value("start_b").toDouble(), m.value("end_b").toDouble()},
        m.value("mean_similarity").toDouble(), m.value("minimum_similarity").toDouble(), m.value("frame_count").toInt(),
        m.value("offset_frames").toInt(), m.value("seed_count").toInt(), kind == "intro" ? SegmentType::Intro : SegmentType::Credits, m.value("evidence").toMap()};
}
std::vector<size_t> refinementOrder(const std::vector<AudioRecurrence> &found)
{
    std::vector<size_t> order(found.size());for(size_t i=0;i<order.size();++i)order[i]=i;
    std::stable_sort(order.begin(),order.end(),[&](size_t x,size_t y){return found[x].frameCount>found[y].frameCount;});return order;
}
QVariantList refinementWindows(const std::vector<AudioRecurrence> &found,const std::vector<size_t> &order,int limit,const TimeRange &a,const TimeRange &b)
{
    if(found.empty())return {};
    double sa=a.end,ea=a.start,sb=b.end,eb=b.start;
    for(size_t i=0;i<std::min(order.size(),size_t(limit));++i) {
        const auto &m=found[order[i]];sa=std::min(sa,m.a.start);ea=std::max(ea,m.a.end);sb=std::min(sb,m.b.start);eb=std::max(eb,m.b.end);
    }
    const auto layout=audioFingerprintLayout();const double margin=double(layout.contextSamples+layout.hopSamples)/layout.sampleRate+.05;
    const auto region=[&](const TimeRange &window,double first,double last) {
        const double start=std::max(window.start,window.start+std::floor((first-margin-window.start)/32.)*32.);
        const double end=std::min(window.end,window.start+std::ceil((last+margin-window.start)/32.)*32.);
        return QVariantMap{{"start",start},{"end",end}};
    };
    return {region(a,sa,ea),region(b,sb,eb)};
}
int invoke(const char *operation, const uint8_t *data, uint64_t length, const IntrovertDetectorHostV1 *host)
{
    if (!host || host->size < sizeof(IntrovertDetectorHostV1) || host->interface_version != algorithmInterfaceVersion || !host->write_result
        || !host->cancelled || !data || !operation || length > uint64_t(algorithmMessageLimit)) return 0;
    QVariantMap response; AudioMatchStatistics stats;
    const auto cancelled = [&] { if (host->cancelled(host->context)) throw Cancelled(); };
    try {
        checkCancelled(cancelled); const auto request = parseAlgorithmMessage(QByteArray(reinterpret_cast<const char *>(data), qsizetype(length)));
        const auto p = parameters(request.value("parameters").toMap()); QVariantMap result;
        const auto effort=effortPolicy(request.value("effort").toString());
        if (std::strcmp(operation, "describe") == 0) {
            const auto l = audioFingerprintLayout();
            result = {{"parameters", p.diagnostic()}, {"extractor", audioFingerprintExtractorId}, {"extractor_version", audioFingerprintExtractorVersion}, {"schema", "1"},
                {"configuration", audioFeatureConfiguration(p)}, {"sample_rate", l.sampleRate}, {"hop_samples", l.hopSamples}, {"context_samples", l.contextSamples}, {"columns", qulonglong(audioFeatureColumns)},
                {"finalization", "segments-v1"}, {"visual",visualCreditsDescription()},{"parallel_matches",true},{"parallel_extraction",true},{"refinement_sample_rate",8000}};
            auto visual=result.value("visual").toMap();
            visual["edge_radius_seconds"]=effort.value("visual_radius");visual["frames_per_second"]=effort.value("visual_fps");
            visual["refinement_frames_per_second"]=effort.value("visual_dense_fps");visual["maximum_refinement_candidates"]=effort.value("visual_candidates");
            result["visual"]=visual;result["effort"]=effort;result["parallel_visual"]=true;result["scoped_refinement"]="pair-window-v1";
        } else if (std::strcmp(operation, "extract") == 0) {
            if (!host->read_pcm) throw std::invalid_argument("PCM reader unavailable");
            const auto kind = request.value("type").toString(); if (kind != "intro" && kind != "credits") throw std::invalid_argument("Invalid feature kind");
            const auto read = [&](qint64 first, qsizetype count) {
                if (count < 0 || count > 262144) throw std::invalid_argument("Invalid PCM block");
                std::vector<qint16> samples(size_t(count), 0);
                if (host->read_pcm(host->context, 0, uint32_t(audioFingerprintLayout().sampleRate), first, uint32_t(count), samples.data()) != count)
                    throw DetectionError("algorithm_pcm_failed", "Cannot read the scoped audio window.");
                return samples;
            };
            result = featureMessage(*fingerprintAudio(request.value("episode").toString(), kind == "intro" ? SegmentType::Intro : SegmentType::Credits,
                {request.value("start").toDouble(), request.value("end").toDouble()}, request.value("samples").toLongLong(), read, cancelled, p));
        } else if (std::strcmp(operation, "validate") == 0) {
            validateAudioFingerprint(*featureFromMessage(request.value("track").toMap()), cancelled); result["valid"] = true;
        } else if (std::strcmp(operation, "match") == 0 || std::strcmp(operation,"refine_matches")==0) {
            const auto a = featureFromMessage(request.value("a").toMap()), b = featureFromMessage(request.value("b").toMap());
            const bool scoped=std::strcmp(operation,"refine_matches")==0;
            const ScopedAudioReader read = [&](uint32_t source, uint32_t rate, qint64 first, uint32_t count) {
                if (!host->read_pcm) throw DetectionError("refinement_unavailable", "No scoped PCM reader.");
                std::vector<qint16> pcm(count);
                const auto got=host->read_pcm(host->context, source, rate, first, count, pcm.data());
                if (got < 0 || uint64_t(got) > count)
                    throw DetectionError("refinement_incomplete_audio", "Incomplete scoped PCM audio.");
                pcm.resize(size_t(got));
                return pcm;
            };
            std::vector<AudioRecurrence> found;
            if(scoped) {
                const auto raw=request.value("matches").toList();if(raw.size()>p.maximumMatches)throw std::invalid_argument("Too many raw matches.");
                for(const auto &entry:raw) {
                    auto m=recurrence(entry.toMap());
                    if(m.episodeA!=a->episodeId || m.episodeB!=b->episodeId || m.type!=a->type || m.type!=b->type || !a->window.contains(m.a) || !b->window.contains(m.b))
                        throw std::invalid_argument("Invalid scoped raw match.");
                    found.push_back(std::move(m));
                }
            } else found=matchAudio(*a,*b,cancelled,p,&stats);
            const auto order=refinementOrder(found);const auto windows=refinementWindows(found,order,effort.value("audio_candidates").toInt(),a->window,b->window);
            const auto wa=scoped && !windows.empty()?TimeRange(windows[0].toMap().value("start").toDouble(),windows[0].toMap().value("end").toDouble()):a->window;
            const auto wb=scoped && !windows.empty()?TimeRange(windows[1].toMap().value("start").toDouble(),windows[1].toMap().value("end").toDouble()):b->window;
            if (scoped || request.value("refine").toBool()) for (size_t i=0;i<order.size();++i) {
                auto &match=found[order[i]];
                if (i<size_t(effort.value("audio_candidates").toInt())) match=refineAudioMatch(match,wa,wb,read,cancelled,
                    {effort.value("audio_comparisons").toLongLong(),effort.value("audio_peaks").toInt()});
                else match.evidence["refinement"]=QVariantMap{{"status","unresolved"},{"reason","pair_refinement_limit"}};
            }
            QVariantList matches; for (const auto &match : found) matches.append(match.diagnostic());
            result = {{"matches", matches}, {"statistics", stats.diagnostic()}, {"insufficient_evidence", !stats.informativeFramesA || !stats.informativeFramesB
                || stats.distinctPointsA < p.minimumDistinctPoints || stats.distinctPointsB < p.minimumDistinctPoints}};
            result["refinement_windows"]=windows;
        } else if (std::strcmp(operation, "select") == 0) {
            std::vector<AudioRecurrence> matches; for (const auto &m : request.value("matches").toList()) { checkCancelled(cancelled); matches.push_back(recurrence(m.toMap())); }
            QVariantList selected, decisions; for (const auto &s : selectAudioSegments(matches, cancelled, &decisions)) selected.append(s.diagnostic()); result["selected"] = selected; result["decisions"] = decisions;
        } else if (std::strcmp(operation, "observe_video_frame") == 0) {
            result["values"]=observeCreditFrame(request.value("pixels").toByteArray(),request.value("width").toInt(),request.value("height").toInt(),cancelled);
        } else if (std::strcmp(operation, "visual_candidates") == 0) {
            result=findVisualCredits(request.value("observations").toList(),request.value("step").toDouble(),{request.value("start").toDouble(),request.value("end").toDouble()},cancelled);
        } else if (std::strcmp(operation, "refine_visual") == 0) {
            result["candidates"]=refineVisualCredits(request.value("candidates").toList(),request.value("observations").toList(),{request.value("start").toDouble(),request.value("end").toDouble()},cancelled);
        } else if (std::strcmp(operation, "fuse") == 0) {
            result=fuseCreditCandidates(request.value("audio_selected").toList(),request.value("matches").toList(),request.value("visual").toMap(),cancelled,effort.value("visual_radius").toDouble());
        } else if (std::strcmp(operation, "finalize") == 0) {
            const auto kind = request.value("segment_type").toString();
            if (kind != "intro" && kind != "credits") throw std::invalid_argument("Invalid finalization kind");
            bool startOk = false, endOk = false;
            const double start = request.value("start").toDouble(&startOk), end = request.value("end").toDouble(&endOk);
            if (!startOk || !endOk || !std::isfinite(start) || !std::isfinite(end) || start < 0 || start >= end)
                throw std::invalid_argument("Invalid measured interval");
            const double duration = request.value("duration").toDouble();
            const bool knownDuration = std::isfinite(duration) && duration > 0;
            if (knownDuration && end > duration) throw std::invalid_argument("Measured interval exceeds EOF");
            const bool preview = request.value("segments").toMap().contains("preview");
            const bool extend = kind == "credits" && !preview && knownDuration && end < duration;
            const auto reason = kind != "credits" ? "not_credits" : preview ? "preview_exists"
                : !knownDuration ? "duration_unavailable" : end == duration ? "already_at_eof" : "no_preview";
            result = {{"start", start}, {"end", extend ? duration : end},
                {"diagnostics", QVariantMap{{"policy", "credits-eof-unless-preview-v1"}, {"status", extend ? "extended" : "unchanged"},
                    {"reason", reason}, {"measured_start", start}, {"measured_end", end}, {"final_start", start},
                    {"final_end", extend ? duration : end}, {"duration", knownDuration ? QVariant(duration) : QVariant{}}, {"preview_exists", preview}}}};
        } else throw DetectionError("algorithm_unknown_operation", "Unsupported algorithm operation.");
        checkCancelled(cancelled); response = {{"ok", true}, {"value", result}};
    } catch (const DetectionError &e) { response = {{"ok", false}, {"code", e.code}, {"statistics", stats.diagnostic()}}; }
    catch (...) { response = {{"ok", false}, {"code", "algorithm_failed"}, {"statistics", stats.diagnostic()}}; }
    try {
        const auto bytes = algorithmMessage(response);
        return host->write_result(host->context, reinterpret_cast<const uint8_t *>(bytes.constData()), uint64_t(bytes.size()));
    } catch (...) { return 0; }
}
const IntrovertDetectorApiV1 api{sizeof(IntrovertDetectorApiV1), algorithmInterfaceVersion, metadata, invoke};
}
} // namespace introvert::detection
#if defined(_WIN32)
#define INTROVERT_ALGORITHM_EXPORT __declspec(dllexport)
#else
#define INTROVERT_ALGORITHM_EXPORT __attribute__((visibility("default")))
#endif
extern "C" INTROVERT_ALGORITHM_EXPORT const IntrovertDetectorApiV1 *introvert_detector_api_v1()
{
    return &introvert::detection::api;
}
