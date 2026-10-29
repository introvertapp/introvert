#pragma once

#include "providers/contracts.h"
#include <QMap>
#include <QSet>

namespace introvert::segments {

using SegmentValues = QMap<QString, QPair<double, double>>;
struct CatalogSegments {
    QMap<QString, SegmentValues> values;
    QSet<QString> edited;
};

// The catalog's read/import boundary. Explicit edits and tombstones win across
// every canonical alias. Explicit mutations use SegmentStore.
CatalogSegments loadCatalogSegments(const QString &databasePath, const providers::ShowDetail &detail);

} // namespace introvert::segments
