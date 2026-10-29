#pragma once

#include "feature_cache.h"
#include <QVariantMap>
#include <QStringList>

namespace introvert::detection {

inline constexpr char audioCompositionVersion[] = "audio-fingerprint-v2";
inline constexpr char audioWindowPolicyVersion[] = "hard-audio-windows-v1";
inline constexpr char audioSelectionVersion[] = "supported-audio-match-v4";
inline constexpr qint64 audioSelectionWorkLimit = 8000000;
inline constexpr char audioFingerprintExtractorId[] = "chromaprint-audio";
inline constexpr char audioFingerprintExtractorVersion[] = "chromaprint-1.6.2-test2-v1";
inline constexpr size_t audioFeatureColumns = 3; // Exact low/high 16-bit halves, audibility.
struct AudioFingerprintLayout { int sampleRate, hopSamples, contextSamples; };
AudioFingerprintLayout audioFingerprintLayout();

// Historical duration windows: first five / last three minutes below thirty
// minutes, otherwise first / last ten minutes. No recovery widens these ranges.
QList<ScanWindow> planAudioWindows(double duration, const QList<SegmentType> &types);

// Initial core matching criteria and hard resource bounds, not calibrated
// confidence or semantic intro/credits policies.
struct AudioRecurrenceParameters {
    double minimumSeconds = 5., silenceFloorDbfs = -60.;
    int seedBitErrors = 3, frameBitErrors = 8, minimumSeeds = 3, minimumDistinctPoints = 8, maximumSeedBucketSize = 128;
    qint64 maximumComparisons = 8000000, maximumTrackBytes = 256 * 1024 * 1024;
    int maximumMatches = 4096, feedSamples = 65536;
    double maximumInterruptionSeconds = .75, minimumAnchorSeconds = 1.5, minimumCoverage = .9;
    void validate() const;
    QVariantMap diagnostic() const;
};

struct AudioMatchStatistics {
    qint64 seedComparisons = 0, verificationComparisons = 0, suppressedSeedLookups = 0;
    int candidateOffsets = 0, verifiedOffsets = 0, informativeFramesA = 0, informativeFramesB = 0;
    int distinctPointsA = 0, distinctPointsB = 0, lowDiversityRuns = 0;
    QString exhaustedBudget;
    QVariantMap diagnostic() const;
};

struct AudioRecurrence {
    QString episodeA, episodeB;
    TimeRange a, b;
    double meanSimilarity, minimumSimilarity;
    int frameCount, offsetFrames, seedCount;
    SegmentType type = SegmentType::Intro;
    QVariantMap evidence;
    QVariantMap diagnostic() const;
};

struct AudioSegment {
    QString episodeId;
    SegmentType type;
    TimeRange range;
    QString peer, matchId;
    int frameCount;
    double minimumSimilarity, meanSimilarity;
    QStringList supportingPeers;
    QVariantMap evidence;
    QVariantMap diagnostic() const;
};

// Rank measured intervals by distinct-peer support, retaining the representative's
// actual bounds. Comparable disjoint candidates remain unassigned as ambiguous.
// A selected fragment can promote an enclosing measured match only with repeated
// boundary support and majority interval coverage from independent peer episodes.
// A dominant intro-duration cohort also checks short outliers, recovering only
// independently verified, coherently aligned fragments with measured bounds.
std::vector<AudioSegment> selectAudioSegments(const std::vector<AudioRecurrence> &matches, const Cancellation &cancel = {}, QVariantList *diagnostics = nullptr);

// Pure pair matcher. Intervals retain their measured frame boundaries; no
// invented gap scores, semantic labels or boundary correction. Supported short
// interruptions remain explicit in the returned evidence.
std::vector<AudioRecurrence> matchAudio(const FeatureTrack &a, const FeatureTrack &b,
    const Cancellation &cancel = {}, const AudioRecurrenceParameters &parameters = {}, AudioMatchStatistics *statistics = nullptr);

QVariantList audioFeatureConfiguration(const AudioRecurrenceParameters &parameters = {});
void validateAudioFingerprint(const FeatureTrack &track, const Cancellation &cancel = {});

// Pure algorithm entry: decoding, PCM storage and cache ownership stay in the host.
using PcmReader = std::function<std::vector<qint16>(qint64, qsizetype)>;
std::shared_ptr<const FeatureTrack> fingerprintAudio(const QString &episode, SegmentType type, const TimeRange &window,
    qint64 sampleCount, const PcmReader &readSamples, const Cancellation &cancel = {}, const AudioRecurrenceParameters &parameters = {});


} // namespace introvert::detection
