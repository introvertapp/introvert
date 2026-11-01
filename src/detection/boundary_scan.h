#pragma once
#include "algorithm_client.h"
#include "media_probe.h"

namespace introvert::detection {
QVariantList parseBoundaryStatistics(const QByteArray &output,const TimeRange &window,const Cancellation &cancel={});
QVariantMap scanAlgorithmBoundary(const QVariantMap &description,ProcessRunner &runner,const MediaInfo &media,
    const QString &episode,const QString &job,const QString &ffmpeg,const ScanWindow &hardWindow,double reference,
    const Cancellation &cancel={});
}
