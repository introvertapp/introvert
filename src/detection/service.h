#pragma once

#include "job_store.h"
#include "media_engine.h"
#include "segments/segment_store.h"
#include "core/settings_store.h"

namespace introvert::detection {
using DetectionUpdates = QMap<QString, QPair<segments::SegmentValues, bool>>;
class DetectionService final {
public:
    using Config = std::function<providers::ProviderConfig(const ProviderSettings &, const providers::Cancellation &)>;
    using Completion = std::function<QVariantMap(const QVariantMap &)>;
    DetectionService(DetectionJobStore store, segments::SegmentStore segments, std::shared_ptr<MediaDetectionRunner> runner, Config config, Completion completion = {});
    DetectionUpdates run(const QString &jobId, const providers::ShowDetail &detail, int season, const QStringList &kinds,
        const ProviderSettings &saved, const providers::Cancellation &cancel, const std::function<void()> &notify) const;
private:
    void completed(const QString &jobId, const std::function<void()> &notify) const;
    const DetectionJobStore m_store;
    const segments::SegmentStore m_segments;
    const std::shared_ptr<MediaDetectionRunner> m_runner;
    const Config m_config;
    const Completion m_completion;
};
} // namespace introvert::detection
