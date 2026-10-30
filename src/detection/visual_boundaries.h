#pragma once
#include "contracts.h"
#include <QByteArray>
#include <QVariantMap>

namespace introvert::detection {
inline constexpr int boundaryFeatureColumns=96;
QVariantMap visualBoundaryDescription();
QVariantList observeBoundaryFrame(const QByteArray &rgb,int width,int height,const Cancellation &cancel={});
QVariantMap refineIntroVisualBoundaries(const QVariantList &audio,const QVariantMap &visual,
    const Cancellation &cancel={});
}
