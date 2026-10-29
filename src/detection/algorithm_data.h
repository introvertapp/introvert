#pragma once
#include "feature_cache.h"

namespace introvert::detection {
inline constexpr int algorithmInterfaceVersion = 1, algorithmResultSchemaVersion = 1;
inline constexpr qsizetype algorithmMessageLimit = 32 * 1024 * 1024;
QVariantMap featureMessage(const FeatureTrack &track);
std::shared_ptr<const FeatureTrack> featureFromMessage(const QVariantMap &message);
QVariantMap parseAlgorithmMessage(const QByteArray &message);
QByteArray algorithmMessage(const QVariantMap &message);
} // namespace introvert::detection
