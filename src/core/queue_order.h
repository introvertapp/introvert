#pragma once

#include "sqlite.h"
#include <QStringList>

namespace introvert {

// Shared display/scheduling order. Call mutations inside the caller's write
// transaction so claiming, appending and moving cannot overwrite each other.
namespace queueOrder {
void initialize(QSqlDatabase &db);
void append(QSqlDatabase &db, const QString &key);
void remove(QSqlDatabase &db, const QString &key);
void running(QSqlDatabase &db, const QString &key, bool value);
bool move(QSqlDatabase &db, const QString &key, const QString &target, bool after, const QStringList &order = {});
QString claim(QSqlDatabase &db, const QStringList &keys);
}

} // namespace introvert
