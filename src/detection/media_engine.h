#pragma once

#include "library/media_cache.h"
#include "contracts.h"
#include "segments/catalog_segments.h"
#include <functional>

namespace introvert::detection {
using DetectionProgress = std::function<void(const QString &, int, int)>;
struct FinalizedSegment { QPair<double, double> interval; QVariantMap diagnostics; };
struct MediaDetectionOutput {
    QMap<QString, segments::SegmentValues> episodes;
    QVariantMap diagnostics;
    bool failed = false;
    // Retains the job's algorithm handle. Current canonical segments are supplied
    // inside the commit transaction, so rules cannot use a stale Preview state.
    std::function<FinalizedSegment(const QString &, const QString &, const QPair<double, double> &,
        const segments::SegmentValues &, const providers::Cancellation &)> finalize;
};
class MediaDetectionRunner {
public:
    virtual ~MediaDetectionRunner() = default;
    virtual MediaDetectionOutput run(const providers::ProviderConfig &config, const QList<providers::Episode> &episodes, const QStringList &kinds,
        const providers::Cancellation &cancel, const DetectionProgress &progress, const QString &jobId) = 0;
};
class MediaDetectionEngine final : public MediaDetectionRunner {
public:
    MediaDetectionEngine(QString databasePath, library::MediaCache &mediaCache, QString ffmpeg = {}, QString ffprobe = {}, QVariantMap parameters = {}, QString algorithmRoot = {}, QString resources = {});
    MediaDetectionOutput run(const providers::ProviderConfig &config, const QList<providers::Episode> &episodes, const QStringList &kinds,
        const providers::Cancellation &cancel, const DetectionProgress &progress, const QString &jobId) override;
private:
    const QString m_path, m_ffmpeg, m_ffprobe, m_algorithmRoot, m_resourceProfile;
    const QVariantMap m_parameters;
    library::MediaCache &m_cache;
};
} // namespace introvert::detection
