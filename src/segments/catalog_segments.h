#pragma once

#include "providers/contracts.h"
#include "edit_locks.h"
#include <QMap>
#include <QSet>

namespace introvert::segments {

using SegmentValues = QMap<QString, QPair<double, double>>;
struct CatalogSegments {
    QMap<QString, SegmentValues> values;
    QSet<QString> edited;
    QMap<QString,qint64> statusRevisions;
    QSet<qint64> deferredSeasons;
};

// The catalog's read/import boundary. Explicit edits and tombstones win across
// every canonical alias. Explicit mutations use SegmentStore.
CatalogSegments loadCatalogSegments(const QString &databasePath, const providers::ShowDetail &detail,
    const QString &connection = {}, const EditLocks::Lease *reservation = nullptr);

} // namespace introvert::segments
