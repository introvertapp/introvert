#pragma once

#include "contracts.h"
#include <QVariantMap>
#include <cstdint>
#include <vector>

namespace introvert::detection {

struct FingerprintTrack {
    QString episodeId;
    SegmentType type;
    TimeRange window;
    int sampleRate, hopSamples, contextSamples;
    std::vector<uint32_t> points;
    std::vector<unsigned char> audible;
    double hopSeconds() const { return double(hopSamples) / sampleRate; }
};
struct FingerprintMatchingParameters {
    int seedBitErrors = 3, frameBitErrors = 8, minimumSeeds = 3, minimumDistinctPoints = 8, maximumPostingSize = 128;
    double minimumSeconds = 5.;
    qint64 maximumWork = 8000000;
    int maximumMatches = 4096;
    double maximumInterruptionSeconds = 0., minimumAnchorSeconds = 1.5, minimumCoverage = .9;
    void validate() const;
};
struct FingerprintMatchingStatistics {
    qint64 retrievalWork = 0, verificationWork = 0, suppressedLookups = 0;
    int informativeA = 0, informativeB = 0, distinctA = 0, distinctB = 0, candidateOffsets = 0, verifiedOffsets = 0, lowDiversityRuns = 0;
    QString exhaustedBudget;
    QVariantMap diagnostic() const;
};
struct FingerprintMatch {
    int firstA, firstB, count, seeds;
    double meanBitErrors;
    int maximumBitErrors;
    int matchedCount = 0, interruptionCount = 0;
    std::vector<std::pair<int, int>> fragments;
};

// Retrieval proposes offsets; direct bit-distance verification decides each run.
// Failed points are never scored as matches. Optional bounded interruptions
// require verified diverse anchors on both sides and retain their coverage.
std::vector<FingerprintMatch> matchFingerprints(const FingerprintTrack &a, const FingerprintTrack &b,
    const Cancellation &cancel = {}, const FingerprintMatchingParameters &parameters = {}, FingerprintMatchingStatistics *statistics = nullptr);

} // namespace introvert::detection
