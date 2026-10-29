#pragma once

#include "providers/contracts.h"
#include <QSqlDatabase>
#include <QStringList>

namespace introvert::segments {

QStringList showKeys(const providers::ShowSummary &show);
void initializeTables(QSqlDatabase &db);
// The caller owns the transaction containing metadata and alias changes.
void copyAliases(QSqlDatabase &db, const providers::ShowSummary &before, const providers::ShowSummary &after);

} // namespace introvert::segments
