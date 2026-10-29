#pragma once
#include "algorithm_client.h"
#include "audio_decode.h"

namespace introvert::detection {
std::shared_ptr<const FeatureTrack> extractAlgorithmFeatures(Algorithm &algorithm, const QVariantMap &description,
    ProcessRunner &runner, const MediaInfo &media, const QString &episode, const QString &job, const QString &root,
    const QString &ffmpeg, FeatureCache *cache, const Cancellation &cancel, const DiagnosticSink &sink,
    const QVariantMap &parameters, const ScanWindow &window, int refinementRate = 0,
    const std::function<void(DecodedAudio &&)> &retainRefinement = {});
} // namespace introvert::detection
