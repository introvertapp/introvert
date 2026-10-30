#pragma once
#include "algorithm_client.h"
#include "media_probe.h"
namespace introvert::detection {
QVariantMap extractAlgorithmIntroBoundaries(Algorithm &algorithm,const QVariantMap &description,ProcessRunner &runner,
    const MediaInfo &media,const QString &episode,const QString &job,const QString &ffmpeg,const ScanWindow &hardWindow,
    double audioStart,double audioEnd,const Cancellation &cancel={});
QVariantMap extractAlgorithmVideo(Algorithm &algorithm, const QVariantMap &description, ProcessRunner &runner,
    const MediaInfo &media, const QString &episode, const QString &job, const QString &ffmpeg, FeatureCache *cache,
    const ScanWindow &hardWindow, double audioStart, const Cancellation &cancel = {}, const DiagnosticSink &sink = {},
    const std::function<void(const QVariantList &,bool)> &observe = {});
}
