#pragma once
#include "contracts.h"
#include <QVariantMap>

namespace introvert::detection {
QVariantMap boundaryRefinementDescription();
QVariantMap refineBoundary(const QVariantMap &window,const QString &kind,const QString &edge,double reference,
    const Cancellation &cancel={},const QString &constraint={});
QVariantMap fuseBoundaryRefinement(const QVariantList &audio,const QVariantMap &visual,const Cancellation &cancel={});
}
