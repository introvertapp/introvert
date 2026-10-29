#include "audio_recurrence.h"
#include "fingerprint_matching.h"
#include "intro_consistency.h"
#include "candidates.h"
#include <chromaprint.h>
#include <QCryptographicHash>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <tuple>

namespace introvert::detection {
namespace {
using Context = std::unique_ptr<ChromaprintContext, decltype(&chromaprint_free)>;
Context context()
{
    if (QString::fromLatin1(chromaprint_get_version()) != "1.6.2") throw DetectionError("fingerprint_runtime_version", "The fingerprint runtime version is incompatible.");
    Context result(chromaprint_new(CHROMAPRINT_ALGORITHM_TEST2), chromaprint_free);
    if (!result) throw DetectionError("fingerprint_initialization_failed", "Cannot create audio fingerprint state.");
    return result;
}
void require(bool success) { if (!success) throw DetectionError("fingerprint_extraction_failed", "The audio fingerprint runtime could not process this window."); }
void limit() { throw DetectionError("raw_audio_limit_exceeded", "Audio analysis exceeded its configured resource budget."); }
FingerprintMatchingParameters matchingParameters(const AudioRecurrenceParameters &p)
{
    return {p.seedBitErrors, p.frameBitErrors, p.minimumSeeds, p.minimumDistinctPoints, p.maximumSeedBucketSize,
        p.minimumSeconds, p.maximumComparisons, p.maximumMatches, p.maximumInterruptionSeconds, p.minimumAnchorSeconds, p.minimumCoverage};
}
size_t expectedFrames(qint64 samples, const AudioFingerprintLayout &layout)
{
    return samples < layout.contextSamples ? 0 : size_t((samples - layout.contextSamples) / layout.hopSamples + 1);
}
FingerprintTrack unpack(const FeatureTrack &track, const Cancellation &cancel)
{
    const auto layout = audioFingerprintLayout();
    if (track.columns != audioFeatureColumns || track.extractorId != audioFingerprintExtractorId || track.extractorVersion != audioFingerprintExtractorVersion
        || track.rows() > size_t(std::numeric_limits<int>::max())) throw DetectionError("fingerprint_invalid_track", "Invalid fingerprint feature layout.");
    FingerprintTrack result{track.episodeId, track.type, track.window, layout.sampleRate, layout.hopSamples, layout.contextSamples, {}, {}};
    result.points.reserve(track.rows()); result.audible.reserve(track.rows());
    const double origin = track.window.start + double(layout.contextSamples - layout.hopSamples) / (2 * layout.sampleRate);
    for (size_t row = 0; row < track.rows(); ++row) {
        if (row % 256 == 0) checkCancelled(cancel);
        const float low = track.values[row * audioFeatureColumns], high = track.values[row * audioFeatureColumns + 1], audible = track.values[row * audioFeatureColumns + 2];
        if (low < 0 || low > 65535 || low != std::floor(low) || high < 0 || high > 65535 || high != std::floor(high) || (audible != 0 && audible != 1)
            || std::abs(track.timestamps[row] - (origin + row * result.hopSeconds())) > 1e-8)
            throw DetectionError("fingerprint_invalid_track", "Invalid fingerprint values or timebase.");
        result.points.push_back(uint32_t(low) | (uint32_t(high) << 16)); result.audible.push_back(audible != 0);
    }
    if (!track.timestamps.empty() && (double(track.rows() - 1) * layout.hopSamples + layout.contextSamples) / layout.sampleRate > track.window.duration() + 1e-8)
        throw DetectionError("fingerprint_invalid_track", "Fingerprint context exceeds its search window.");
    return result;
}
}
AudioFingerprintLayout audioFingerprintLayout()
{
    static const auto layout = [] {
        auto state = context();
        const int rate = chromaprint_get_sample_rate(state.get()), hop = chromaprint_get_item_duration(state.get()), delay = chromaprint_get_delay(state.get());
        if (rate <= 0 || hop <= 0 || delay < 0 || delay > std::numeric_limits<int>::max() - hop)
            throw DetectionError("fingerprint_runtime_version", "Invalid fingerprint runtime timebase.");
        return AudioFingerprintLayout{rate, hop, delay + hop};
    }();
    return layout;
}
QList<ScanWindow> planAudioWindows(double duration, const QList<SegmentType> &types)
{
    if (!std::isfinite(duration) || duration <= 0 || types.empty()) throw std::invalid_argument("Audio search windows require positive duration and requested types.");
    QList<ScanWindow> windows;
    for (const auto type : types) {
        if (type != SegmentType::Intro && type != SegmentType::Credits) throw std::invalid_argument("Unsupported audio search type.");
        if (std::any_of(windows.cbegin(), windows.cend(), [type](const auto &w) { return w.type == type; })) continue;
        windows.append({type, type == SegmentType::Intro ? TimeRange(0, std::min(duration < 1800. ? 300. : 600., duration))
            : TimeRange(std::max(0., duration - (duration < 1800. ? 180. : 600.)), duration)});
    }
    return windows;
}

void AudioRecurrenceParameters::validate() const
{
    matchingParameters(*this).validate();
    if (!std::isfinite(silenceFloorDbfs) || silenceFloorDbfs > 0 || silenceFloorDbfs < -120 || maximumTrackBytes <= 0 || feedSamples <= 0 || feedSamples > 65536)
        throw std::invalid_argument("Invalid fingerprint extraction parameters.");
}
QVariantMap AudioRecurrenceParameters::diagnostic() const
{
    const auto layout = audioFingerprintLayout();
    return {{"seed_bit_errors", seedBitErrors}, {"frame_bit_errors", frameBitErrors}, {"minimum_seconds", minimumSeconds}, {"minimum_seeds", minimumSeeds},
        {"minimum_distinct_points", minimumDistinctPoints}, {"maximum_seed_bucket_size", maximumSeedBucketSize}, {"silence_floor_dbfs", silenceFloorDbfs},
        {"similarity_domain", "fingerprint_bit_agreement"}, {"maximum_comparisons_per_pair", maximumComparisons}, {"maximum_track_bytes", maximumTrackBytes},
        {"maximum_matches", maximumMatches}, {"maximum_interruption_seconds", maximumInterruptionSeconds}, {"minimum_anchor_seconds", minimumAnchorSeconds}, {"minimum_coverage", minimumCoverage}, {"feed_samples", feedSamples}, {"fingerprint_library", "Chromaprint"}, {"fingerprint_library_version", "1.6.2"},
        {"fingerprint_algorithm", "TEST2"}, {"fingerprint_algorithm_id", CHROMAPRINT_ALGORITHM_TEST2}, {"sample_rate", layout.sampleRate}, {"hop_samples", layout.hopSamples}, {"context_samples", layout.contextSamples},
        {"timestamp_grid", "fingerprint-center-bins-v1"}, {"descriptor_dimensions", qulonglong(audioFeatureColumns)}};
}
QVariantMap AudioRecurrence::diagnostic() const
{
    const QVariantMap identity{{"version", audioCompositionVersion}, {"segment_type", segmentName(type)}, {"episode_a", episodeA}, {"episode_b", episodeB},
        {"start_a", a.start}, {"end_a", a.end}, {"start_b", b.start}, {"end_b", b.end}, {"offset_frames", offsetFrames}};
    auto result = identity;
    result["id"] = QString::fromLatin1(QCryptographicHash::hash(detectorJson(identity), QCryptographicHash::Sha256).toHex());
    result["mean_similarity"] = meanSimilarity; result["minimum_similarity"] = minimumSimilarity;
    result["frame_count"] = frameCount; result["seed_count"] = seedCount; result["evidence"] = evidence;
    return result;
}

QVariantMap AudioMatchStatistics::diagnostic() const
{
    return {{"seed_comparisons", seedComparisons}, {"verification_comparisons", verificationComparisons}, {"suppressed_seed_lookups", suppressedSeedLookups},
        {"candidate_offsets", candidateOffsets}, {"verified_offsets", verifiedOffsets}, {"informative_frames_a", informativeFramesA}, {"informative_frames_b", informativeFramesB},
        {"distinct_points_a", distinctPointsA}, {"distinct_points_b", distinctPointsB}, {"low_diversity_runs", lowDiversityRuns}, {"exhausted_budget", exhaustedBudget}};
}
QVariantMap AudioSegment::diagnostic() const
{
    QVariantList peers; for (const auto &peer : supportingPeers) peers.append(peer);
    return {{"episode", episodeId}, {"segment_type", segmentName(type)}, {"start", range.start}, {"end", range.end},
        {"peer", peer}, {"match_id", matchId}, {"frame_count", frameCount}, {"minimum_similarity", minimumSimilarity}, {"mean_similarity", meanSimilarity},
        {"supporting_episodes", peers}, {"support_count", supportingPeers.size()}, {"evidence", evidence}};
}
std::vector<AudioSegment> selectAudioSegments(const std::vector<AudioRecurrence> &matches, const Cancellation &cancel, QVariantList *diagnostics)
{
    std::map<std::pair<QString, SegmentType>, std::vector<AudioSegment>> groups;
    for (const auto &m : matches) {
        checkCancelled(cancel);
        if (m.episodeA.isEmpty() || m.episodeB.isEmpty() || m.episodeA == m.episodeB || m.frameCount <= 0
            || !std::isfinite(m.meanSimilarity) || m.meanSimilarity < 0 || m.meanSimilarity > 1
            || !std::isfinite(m.minimumSimilarity) || m.minimumSimilarity < 0 || m.minimumSimilarity > m.meanSimilarity)
            throw std::invalid_argument("Invalid audio selection evidence.");
        for (const bool first : {true, false}) {
            AudioSegment c{first ? m.episodeA : m.episodeB, m.type, first ? m.a : m.b, first ? m.episodeB : m.episodeA,
                m.diagnostic().value("id").toString(), m.frameCount, m.minimumSimilarity, m.meanSimilarity, {}, m.evidence};
            groups[{c.episodeId, c.type}].push_back(std::move(c));
        }
    }
    const auto same = [](const AudioSegment &a, const AudioSegment &b) {
        const double overlap = std::min(a.range.end, b.range.end) - std::max(a.range.start, b.range.start);
        return overlap > 0 && overlap / std::max(a.range.duration(), b.range.duration()) >= .65;
    };
    std::vector<AudioSegment> result; qint64 work = 0;
    for (auto &group : groups) {
        auto &candidates = group.second;
        std::vector<double> strengths(candidates.size());
        for (size_t i = 0; i < candidates.size(); ++i) {
            auto &c = candidates[i]; std::map<QString, double> peers;
            for (const auto &other : candidates) {
                if (++work > audioSelectionWorkLimit) throw DetectionError("selection_limit_exceeded", "Audio candidate selection exceeded its work budget.");
                if (work % 256 == 0) checkCancelled(cancel);
                if (!same(c, other)) continue;
                const int verified = other.evidence.value("matched_frame_count", other.frameCount).toInt();
                peers[other.peer] = std::max(peers[other.peer], verified * other.meanSimilarity);
            }
            for (const auto &peer : peers) c.supportingPeers.append(peer.first);
            // Independent peers supply support; duplicate alignments never add votes.
            for (const auto &peer : peers) strengths[i] += peer.second;
            strengths[i] /= std::max(size_t(1), peers.size());
        }
        const auto rank = [&](size_t i) {
            const auto &c = candidates[i];
            return std::make_tuple(c.supportingPeers.size(), strengths[i], c.frameCount, c.minimumSimilarity, c.meanSimilarity, -c.range.start, -c.range.end);
        };
        size_t best = 0;
        for (size_t i = 1; i < candidates.size(); ++i)
            if (rank(i) > rank(best) || (rank(i) == rank(best) && unicodeLess(candidates[i].matchId, candidates[best].matchId))) best = i;
        bool ambiguous = false;
        for (size_t i = 0; i < candidates.size(); ++i)
            if (!same(candidates[best], candidates[i]) && !candidates[best].range.contains(candidates[i].range)
                && candidates[i].supportingPeers.size() == candidates[best].supportingPeers.size()
                && strengths[i] >= .9 * strengths[best]) ambiguous = true;
        const size_t original=best;
        if (!ambiguous) {
            const auto layout=audioFingerprintLayout();
            const double context=double(layout.contextSamples)/layout.sampleRate,hop=double(layout.hopSamples)/layout.sampleRate;
            std::vector<size_t> enclosures;
            for (size_t i=0;i<candidates.size();++i) {
                auto &c=candidates[i];
                if (!c.range.contains(candidates[original].range) || c.range.duration()<=candidates[original].range.duration()+hop) continue;
                QSet<QString> starts,ends; std::map<QString,double> peers;
                std::vector<std::tuple<double,QString,int>> events;
                for (const auto &other:candidates) {
                    if (++work>audioSelectionWorkLimit) throw DetectionError("selection_limit_exceeded","Audio candidate selection exceeded its work budget.");
                    if (work%256==0) checkCancelled(cancel);
                    const double lo=std::max(c.range.start,other.range.start),hi=std::min(c.range.end,other.range.end);
                    const double overlap=std::max(0.,hi-lo);
                    if (overlap/std::min(c.range.duration(),other.range.duration())<.65) continue;
                    const int verified=other.evidence.value("matched_frame_count",other.frameCount).toInt();
                    const double strength=verified*other.meanSimilarity*(same(c,other)?1.:overlap/other.range.duration());
                    peers[other.peer]=std::max(peers[other.peer],strength);
                    if (std::abs(other.range.start-c.range.start)<=context) starts.insert(other.peer);
                    if (std::abs(other.range.end-c.range.end)<=context) ends.insert(other.peer);
                    events.emplace_back(lo,other.peer,1); events.emplace_back(hi,other.peer,-1);
                }
                // The same independent peers must support both outer boundaries.
                // A long isolated match cannot borrow votes from unrelated ends.
                const auto boundaryPeers=starts.intersect(ends).size();
                if (boundaryPeers<2) continue;
                std::sort(events.begin(),events.end());std::map<QString,int> active;
                int activePeers=0;double last=c.range.start,covered=0;
                for (const auto &event:events) {
                    if (++work>audioSelectionWorkLimit) throw DetectionError("selection_limit_exceeded","Audio candidate selection exceeded its work budget.");
                    if (work%256==0) checkCancelled(cancel);
                    const auto time=std::get<0>(event);
                    if (activePeers>=2) covered+=time-last;
                    auto &count=active[std::get<1>(event)];const bool wasActive=count>0;
                    count+=std::get<2>(event);activePeers+=int(count>0)-int(wasActive);last=time;
                }
                const double coverage=covered/c.range.duration();
                if (coverage<.65) continue;
                c.supportingPeers.clear();strengths[i]=0;
                for (const auto &peer:peers) {c.supportingPeers.append(peer.first);strengths[i]+=peer.second;}
                strengths[i]/=peers.size();
                if (rank(i)<=rank(original)) continue;
                enclosures.push_back(i);
                c.evidence["selection_support"]=QVariantMap{{"method","supported-enclosure-v1"},{"from_match_id",candidates[original].matchId},
                    {"from_start",candidates[original].range.start},{"from_end",candidates[original].range.end},
                    {"boundary_peer_count",boundaryPeers},{"consensus_coverage",coverage}};
                if (rank(i)>rank(best) || (rank(i)==rank(best) && unicodeLess(c.matchId,candidates[best].matchId))) best=i;
            }
            // Competing, non-nested enclosures do not resolve the original edge.
            for (const auto i:enclosures) if (!same(candidates[best],candidates[i])
                && !candidates[best].range.contains(candidates[i].range) && !candidates[i].range.contains(candidates[best].range)
                && candidates[i].supportingPeers.size()==candidates[best].supportingPeers.size() && strengths[i]>=.9*strengths[best]) {best=original;break;}
        }
        if (diagnostics) diagnostics->append(QVariantMap{{"episode", group.first.first}, {"segment_type", segmentName(group.first.second)},
            {"status", ambiguous ? "ambiguous" : "selected"}, {"candidates", int(candidates.size())}, {"support_count", candidates[best].supportingPeers.size()},
            {"rule", audioSelectionVersion},{"enclosure_promoted",best!=original}, {"work", work}});
        if (!ambiguous) result.push_back(candidates[best]);
    }
    checkIntroConsistency(matches,result,cancel,work,diagnostics);
    checkCancelled(cancel); return result;
}
std::vector<AudioRecurrence> matchAudio(const FeatureTrack &a, const FeatureTrack &b, const Cancellation &cancel,
    const AudioRecurrenceParameters &p, AudioMatchStatistics *statistics)
{
    p.validate(); checkCancelled(cancel); const auto first = unpack(a, cancel), second = unpack(b, cancel);
    AudioMatchStatistics local; auto &stats = statistics ? *statistics : local; stats = {};
    FingerprintMatchingStatistics measured;
    const auto update = [&] {
        stats.seedComparisons = measured.retrievalWork; stats.verificationComparisons = measured.verificationWork; stats.suppressedSeedLookups = measured.suppressedLookups;
        stats.candidateOffsets = measured.candidateOffsets; stats.verifiedOffsets = measured.verifiedOffsets; stats.informativeFramesA = measured.informativeA; stats.informativeFramesB = measured.informativeB;
        stats.distinctPointsA = measured.distinctA; stats.distinctPointsB = measured.distinctB; stats.lowDiversityRuns = measured.lowDiversityRuns; stats.exhaustedBudget = measured.exhaustedBudget;
    };
    std::vector<FingerprintMatch> found;
    try { found = matchFingerprints(first, second, cancel, matchingParameters(p), &measured); }
    catch (...) { update(); throw; }
    update(); std::vector<AudioRecurrence> result;
    for (const auto &run : found) {
        checkCancelled(cancel);
        const TimeRange rangeA(a.timestamps[size_t(run.firstA)], a.timestamps[size_t(run.firstA + run.count - 1)] + first.hopSeconds());
        const TimeRange rangeB(b.timestamps[size_t(run.firstB)], b.timestamps[size_t(run.firstB + run.count - 1)] + second.hopSeconds());
        if (!a.window.contains(rangeA) || !b.window.contains(rangeB)) throw DetectionError("fingerprint_invalid_track", "Fingerprint match exceeds its search window.");
        QVariantList fragments;
        for (const auto &fragment : run.fragments) fragments.append(QVariantMap{{"first_frame", fragment.first}, {"frames", fragment.second}});
        result.push_back({a.episodeId, b.episodeId, rangeA, rangeB, 1. - run.meanBitErrors / 32., 1. - run.maximumBitErrors / 32., run.count, run.firstB - run.firstA, run.seeds, a.type,
            {{"matched_frame_count", run.matchedCount}, {"interruption_count", run.interruptionCount}, {"coverage", double(run.matchedCount) / run.count}, {"fragments", fragments}}});
    }
    return result;
}

QVariantList audioFeatureConfiguration(const AudioRecurrenceParameters &p)
{
    const auto layout = audioFingerprintLayout();
    QVariantList configuration;
    const auto add = [&](const QString &key, const QVariant &value) { configuration.append(QVariant(QVariantList{key, value})); };
    add("library", "Chromaprint"); add("library_version", "1.6.2"); add("algorithm", "TEST2"); add("algorithm_id", CHROMAPRINT_ALGORITHM_TEST2); add("sample_rate", layout.sampleRate);
    add("hop_samples", layout.hopSamples); add("context_samples", layout.contextSamples); add("grid", "fingerprint-center-bins-v1");
    add("encoding", "low16-high16-audible-f32-v1"); add("silence_floor_dbfs", p.silenceFloorDbfs); add("window_policy", audioWindowPolicyVersion);
    add("audio_decode_spec", audioDecodeSpecVersion); add("channels", 1); add("pcm", "s16le");
    return configuration;
}
void validateAudioFingerprint(const FeatureTrack &track, const Cancellation &cancel) { (void)unpack(track, cancel); }

std::shared_ptr<const FeatureTrack> fingerprintAudio(const QString &episode, SegmentType type, const TimeRange &allowed,
    qint64 sampleCount, const PcmReader &readSamples, const Cancellation &cancel, const AudioRecurrenceParameters &p)
{
    p.validate(); checkCancelled(cancel); const auto layout = audioFingerprintLayout();
    const qint64 requestedSamples = qint64(std::floor(allowed.duration() * layout.sampleRate + 1e-8));
    const auto expected = expectedFrames(requestedSamples, layout);
    if (sampleCount < 0 || std::abs(double(sampleCount) / layout.sampleRate - allowed.duration()) > .05)
        throw DetectionError("raw_audio_incomplete_chunk", "Decoded audio does not cover its fingerprint window.");
    if (expected > size_t(p.maximumTrackBytes / qint64(sizeof(double) + audioFeatureColumns * sizeof(float)))) limit();
    std::vector<double> times; std::vector<float> values;
    if (expected) {
        auto state = context(); require(chromaprint_start(state.get(), layout.sampleRate, 1));
        for (qint64 first = 0; first < sampleCount; first += p.feedSamples) {
            checkCancelled(cancel); const auto count = qsizetype(std::min(qint64(p.feedSamples), sampleCount - first));
            const auto samples = readSamples(first, count); require(chromaprint_feed(state.get(), samples.data(), int(samples.size())));
        }
        checkCancelled(cancel); require(chromaprint_finish(state.get()));
        uint32_t *raw = nullptr; int count = 0; require(chromaprint_get_raw_fingerprint(state.get(), &raw, &count));
        std::unique_ptr<uint32_t, decltype(&chromaprint_dealloc)> owned(raw, chromaprint_dealloc);
        const auto complete = expectedFrames(std::min(requestedSamples, sampleCount), layout);
        if (count < 0 || size_t(count) != expectedFrames(sampleCount, layout) || complete > size_t(count) || expected - complete > 1)
            throw DetectionError("fingerprint_incomplete", "Fingerprint extraction does not cover its expected frame grid.");
        times.reserve(complete); values.reserve(complete * audioFeatureColumns);
        const double origin = allowed.start + double(layout.contextSamples - layout.hopSamples) / (2 * layout.sampleRate);
        const double floor = std::pow(10., p.silenceFloorDbfs / 10.);
        // The reported bin is centered on the complete fingerprint context.
        // Measure its audibility without resetting the fingerprint state.
        for (size_t first = 0; first < complete; first += 64) {
            checkCancelled(cancel); const auto end = std::min(complete, first + 64);
            const qint64 start = (layout.contextSamples - layout.hopSamples) / 2 + qint64(first) * layout.hopSamples;
            const auto samples = readSamples(start, qsizetype((end - first) * size_t(layout.hopSamples)));
            for (size_t row = first; row < end; ++row) {
                double energy = 0.; for (int i = 0; i < layout.hopSamples; ++i) { const double value = samples[(row - first) * size_t(layout.hopSamples) + size_t(i)] / 32768.; energy += value * value; }
                times.push_back(origin + double(row) * layout.hopSamples / layout.sampleRate);
                values.push_back(float(raw[row] & 65535)); values.push_back(float(raw[row] >> 16)); values.push_back(energy / layout.hopSamples >= floor ? 1.f : 0.f);
            }
        }
    }
    return std::make_shared<FeatureTrack>(episode, type, allowed, "audio-fingerprint:" + episode, audioFingerprintExtractorId,
        audioFingerprintExtractorVersion, "1", std::move(times), std::move(values), audioFeatureColumns);
}

} // namespace introvert::detection
