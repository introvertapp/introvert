#include "fingerprint_matching.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <set>

namespace introvert::detection {
namespace {
int bitCount(uint32_t value)
{
    value -= (value >> 1) & 0x55555555u;
    value = (value & 0x33333333u) + ((value >> 2) & 0x33333333u);
    return int((((value + (value >> 4)) & 0x0f0f0f0fu) * 0x01010101u) >> 24);
}
void validate(const FingerprintTrack &track)
{
    if (track.sampleRate <= 0 || track.hopSamples <= 0 || track.contextSamples < track.hopSamples
        || track.points.size() != track.audible.size() || track.points.size() > size_t(std::numeric_limits<int>::max()))
        throw DetectionError("fingerprint_invalid_track", "Invalid fingerprint track.");
    if (!track.points.empty() && (double(track.points.size() - 1) * track.hopSamples + track.contextSamples) / track.sampleRate > track.window.duration() + 1e-8)
        throw DetectionError("fingerprint_invalid_track", "Fingerprint context exceeds its hard search window.");
}
}
void FingerprintMatchingParameters::validate() const
{
    if (seedBitErrors < 0 || seedBitErrors > 3 || frameBitErrors < seedBitErrors || frameBitErrors > 15 || minimumSeeds <= 0
        || minimumDistinctPoints < 2 || maximumPostingSize <= 0 || maximumPostingSize > 1024 || !std::isfinite(minimumSeconds) || minimumSeconds <= 0
        || maximumWork <= 0 || maximumMatches <= 0 || !std::isfinite(maximumInterruptionSeconds) || maximumInterruptionSeconds < 0 || maximumInterruptionSeconds > 2.
        || !std::isfinite(minimumAnchorSeconds) || minimumAnchorSeconds <= 0 || !std::isfinite(minimumCoverage) || minimumCoverage < .8 || minimumCoverage > 1.)
        throw std::invalid_argument("Invalid fingerprint matching parameters.");
}
QVariantMap FingerprintMatchingStatistics::diagnostic() const
{
    return {{"seed_comparisons", retrievalWork}, {"verification_comparisons", verificationWork}, {"suppressed_seed_lookups", suppressedLookups},
        {"informative_frames_a", informativeA}, {"informative_frames_b", informativeB}, {"distinct_points_a", distinctA}, {"distinct_points_b", distinctB},
        {"candidate_offsets", candidateOffsets}, {"verified_offsets", verifiedOffsets}, {"low_diversity_runs", lowDiversityRuns}, {"exhausted_budget", exhaustedBudget}};
}
std::vector<FingerprintMatch> matchFingerprints(const FingerprintTrack &a, const FingerprintTrack &b, const Cancellation &cancel,
    const FingerprintMatchingParameters &p, FingerprintMatchingStatistics *statistics)
{
    p.validate(); checkCancelled(cancel); validate(a); validate(b);
    if (a.episodeId == b.episodeId || a.type != b.type || a.sampleRate != b.sampleRate || a.hopSamples != b.hopSamples || a.contextSamples != b.contextSamples)
        throw std::invalid_argument("Fingerprint comparison requires distinct episodes with the same type and timebase.");
    FingerprintMatchingStatistics local; auto &stats = statistics ? *statistics : local; stats = {};
    const auto consume = [&](bool verify) {
        if (verify) ++stats.verificationWork; else ++stats.retrievalWork;
        if (stats.retrievalWork + stats.verificationWork > p.maximumWork) {
            stats.exhaustedBudget = verify ? "verification_comparisons" : "seed_comparisons";
            throw DetectionError("raw_audio_limit_exceeded", "Fingerprint matching exceeded its work budget.");
        }
        if ((stats.retrievalWork + stats.verificationWork) % 256 == 0) checkCancelled(cancel);
    };
    const auto summarize = [&](const FingerprintTrack &track, int &informative, int &distinct) {
        std::set<uint32_t> unique;
        for (size_t i = 0; i < track.points.size(); ++i) {
            if (i % 256 == 0) checkCancelled(cancel);
            if (track.audible[i]) { ++informative; unique.insert(track.points[i]); }
        }
        distinct = int(unique.size());
    };
    summarize(a, stats.informativeA, stats.distinctA); summarize(b, stats.informativeB, stats.distinctB);
    if (stats.distinctA < p.minimumDistinctPoints || stats.distinctB < p.minimumDistinctPoints) return {};
    struct Bucket { int count = 0; std::vector<int> rows; };
    std::array<Bucket, 1024> index;
    for (size_t row = 0; row < b.points.size(); ++row) {
        if (row % 256 == 0) checkCancelled(cancel);
        if (!b.audible[row]) continue;
        for (int band = 0; band < 4; ++band) {
            auto &bucket = index[size_t(band * 256 + ((b.points[row] >> (band * 8)) & 255))];
            if (++bucket.count <= p.maximumPostingSize) bucket.rows.push_back(int(row)); else bucket.rows.clear();
        }
    }
    std::map<int, std::vector<int>> votes;
    for (size_t row = 0; row < a.points.size(); ++row) {
        if (row % 256 == 0) checkCancelled(cancel);
        if (!a.audible[row]) continue;
        std::vector<int> references;
        for (int band = 0; band < 4; ++band) {
            consume(false); const auto &bucket = index[size_t(band * 256 + ((a.points[row] >> (band * 8)) & 255))];
            if (bucket.count > p.maximumPostingSize) { ++stats.suppressedLookups; continue; }
            for (const int target : bucket.rows) { consume(false); references.push_back(target); }
        }
        std::sort(references.begin(), references.end()); references.erase(std::unique(references.begin(), references.end()), references.end());
        for (const int target : references) {
            consume(false);
            if (bitCount(a.points[row] ^ b.points[size_t(target)]) <= p.seedBitErrors) votes[target - int(row)].push_back(int(row));
        }
    }
    std::vector<FingerprintMatch> result;
    for (const auto &entry : votes) {
        const int offset = entry.first; const auto &anchors = entry.second;
        checkCancelled(cancel); if (anchors.size() < size_t(p.minimumSeeds)) continue;
        ++stats.candidateOffsets; ++stats.verifiedOffsets;
        const int begin = std::max(0, -offset), end = std::min(int(a.points.size()), int(b.points.size()) - offset);
        std::vector<unsigned char> visited(size_t(end - begin), 0);
        std::vector<FingerprintMatch> fragments;
        const auto score = [&](int row) {
            consume(true); visited[size_t(row - begin)] = 1;
            return !a.audible[size_t(row)] || !b.audible[size_t(row + offset)] ? 33 : bitCount(a.points[size_t(row)] ^ b.points[size_t(row + offset)]);
        };
        for (const int anchor : anchors) {
            if (visited[size_t(anchor - begin)]) continue;
            const int initial = score(anchor); if (initial > p.frameBitErrors) continue;
            int first = anchor, last = anchor + 1, worst = initial; qint64 sum = initial;
            while (first > begin && !visited[size_t(first - 1 - begin)]) {
                const int value = score(first - 1); if (value > p.frameBitErrors) break;
                --first; sum += value; worst = std::max(worst, value);
            }
            while (last < end && !visited[size_t(last - begin)]) {
                const int value = score(last); if (value > p.frameBitErrors) break;
                ++last; sum += value; worst = std::max(worst, value);
            }
            const int count = last - first;
            if (count * a.hopSeconds() < (p.maximumInterruptionSeconds > 0 ? std::min(p.minimumSeconds, p.minimumAnchorSeconds) : p.minimumSeconds)) continue;
            const int seeds = int(std::lower_bound(anchors.begin(), anchors.end(), last) - std::lower_bound(anchors.begin(), anchors.end(), first));
            if (seeds < p.minimumSeeds) continue;
            std::set<uint32_t> distinctA, distinctB;
            for (int row = first; row < last; ++row) {
                if (row % 256 == 0) checkCancelled(cancel);
                distinctA.insert(a.points[size_t(row)]); distinctB.insert(b.points[size_t(row + offset)]);
            }
            if (std::min(distinctA.size(), distinctB.size()) < size_t(p.minimumDistinctPoints)) { ++stats.lowDiversityRuns; continue; }
            fragments.push_back({first, first + offset, count, seeds, double(sum) / count, worst, count, 0, {{first, count}}});
            if (fragments.size() > size_t(p.maximumMatches) * 4) { stats.exhaustedBudget = "fragment_count"; throw DetectionError("raw_audio_limit_exceeded", "Fingerprint fragment count exceeded its budget."); }
        }
        std::sort(fragments.begin(), fragments.end(), [](const auto &x, const auto &y) { return x.firstA < y.firstA; });
        const auto append = [&](const FingerprintMatch &run) {
            if (run.matchedCount * a.hopSeconds() < p.minimumSeconds) return;
            result.push_back(run);
            if (result.size() > size_t(p.maximumMatches)) { stats.exhaustedBudget = "match_count"; throw DetectionError("raw_audio_limit_exceeded", "Fingerprint match count exceeded its budget."); }
        };
        if (!fragments.empty()) {
            auto run = fragments.front();
            for (size_t f = 1; f < fragments.size(); ++f) {
                const auto &next = fragments[f]; const int gap = next.firstA - (run.firstA + run.count);
                const int span = next.firstA + next.count - run.firstA;
                bool supported = gap > 0 && gap * a.hopSeconds() <= p.maximumInterruptionSeconds
                    && double(run.matchedCount + next.matchedCount) / span >= p.minimumCoverage;
                for (int row = run.firstA + run.count; supported && row < next.firstA; ++row) {
                    consume(true); supported = a.audible[size_t(row)] && b.audible[size_t(row + offset)];
                }
                if (supported) {
                    run.meanBitErrors = (run.meanBitErrors * run.matchedCount + next.meanBitErrors * next.matchedCount) / (run.matchedCount + next.matchedCount);
                    run.matchedCount += next.matchedCount; run.count = span; run.seeds += next.seeds;
                    run.maximumBitErrors = std::max(run.maximumBitErrors, next.maximumBitErrors); ++run.interruptionCount;
                    run.fragments.insert(run.fragments.end(), next.fragments.begin(), next.fragments.end());
                } else { append(run); run = next; }
            }
            append(run);
        }
    }
    checkCancelled(cancel); return result;
}
} // namespace introvert::detection
