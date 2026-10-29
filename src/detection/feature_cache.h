#pragma once

#include "features.h"
#include <memory>

namespace introvert::detection {
struct FeatureCacheRequest {
    QString key, pathDigest;
    qint64 fileSize, modifiedTimeNs;
    QString episode;
    SegmentType type;
    TimeRange window;
    std::optional<qint64> streamIndex;
    QString extractorId, extractorVersion, schemaVersion;
    QVariantList configuration;
    QString storageKey() const;
    QByteArray descriptor() const;
};
FeatureCacheRequest buildFeatureCacheRequest(const MediaInfo &media, const QString &episode, SegmentType type, const TimeRange &window,
    const QString &extractorId, const QString &extractorVersion, const QString &schemaVersion, QVariantList configuration, const QString &streamKind = "audio");
QByteArray detectorJson(const QVariant &value, bool compact = true);
QVariantMap cacheOperation(const FeatureCacheRequest &request, const QString &status, qint64 bytes, double seconds, const QVariant &reason = {});
struct FeatureCacheRead { std::shared_ptr<const FeatureTrack> track; QVariantMap operation; };
struct FeatureCacheWrite { std::shared_ptr<const FeatureTrack> track; QVariantMap operation; };
class FeatureCache {
public:
    virtual ~FeatureCache() = default;
    virtual FeatureCacheRead read(const FeatureCacheRequest &request) = 0;
    virtual FeatureCacheWrite write(const FeatureCacheRequest &request, std::shared_ptr<const FeatureTrack> track) = 0;
};
struct EncodedFeatures { QByteArray timestamps, values; };
EncodedFeatures encodeFeatures(const FeatureTrack &track);
std::shared_ptr<const FeatureTrack> decodeFeatures(const FeatureCacheRequest &request, const EncodedFeatures &encoded);
} // namespace introvert::detection
