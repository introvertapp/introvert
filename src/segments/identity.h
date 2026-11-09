#pragma once

#include "providers/contracts.h"
#include <QSqlDatabase>
#include <QStringList>

namespace introvert::segments {

QStringList showKeys(const providers::ShowSummary &show);
// The existing SQL tables require two coordinates. Movies use a reserved
// coordinate under a distinct key, with no season or episode in the domain model.
constexpr qint64 movieCoordinate = -1;
// The original range uses the bare kind. Additional movie ranges use kind:instance.
QString segmentKind(const QString &key);
bool validSegmentKey(const QString &key, bool movie);
std::optional<QPair<qint64, qint64>> segmentCoordinates(const providers::ShowSummary &show, const providers::Episode &episode);
void initializeTables(QSqlDatabase &db);
qint64 currentRevision(QSqlDatabase &db);
// The caller owns the transaction containing metadata and alias changes.
void copyAliases(QSqlDatabase &db, const providers::ShowSummary &before, const providers::ShowSummary &after);

} // namespace introvert::segments
