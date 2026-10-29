#include "sqlite.h"

#include <QFile>
#include <QFileInfo>
#include <QUuid>

namespace introvert::sql {
namespace {
void require(bool condition, const char *message) { if (!condition) throw DatabaseError(message); }
}

QSqlQuery execute(QSqlDatabase &db, const QString &statement, const QVariantList &values)
{
    QSqlQuery query(db);
    require(query.prepare(statement), "Cannot access Introvert settings in app.db. Check database access.");
    for (const auto &value : values) query.addBindValue(value);
    require(query.exec(), "Cannot save or read Introvert settings in app.db. Check database access.");
    return query;
}

QString text(const QString &value) { return value.isNull() ? QStringLiteral("") : value; }

void restrictPermissions(const QString &path)
{
#ifndef Q_OS_WIN
    for (const auto &suffix : {QString(), QStringLiteral("-wal"), QStringLiteral("-shm")}) {
        const auto file = path + suffix;
        if (QFileInfo::exists(file))
            require(QFile::setPermissions(file, QFileDevice::ReadOwner | QFileDevice::WriteOwner) || !QFileInfo::exists(file),
                    "Cannot protect Introvert app.db permissions.");
    }
#else
    Q_UNUSED(path);
#endif
}

Connection::Connection(const QString &path)
    : m_name(QUuid::createUuid().toString(QUuid::WithoutBraces)),
      db(QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_name))
{
    db.setDatabaseName(path);
    db.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
    if (!db.open()) {
        release();
        throw DatabaseError("Cannot open Introvert app.db. Check database access.");
    }
    try {
        execute(db, QStringLiteral("PRAGMA busy_timeout=5000"));
        restrictPermissions(path);
    } catch (...) { release(); throw; }
}

Connection::~Connection() { release(); }
void Connection::release()
{
    db.close(); db = QSqlDatabase(); QSqlDatabase::removeDatabase(m_name);
}

Transaction::Transaction(QSqlDatabase &db) : m_db(db) { execute(m_db, QStringLiteral("BEGIN IMMEDIATE")); }
Transaction::~Transaction() { if (!m_committed) m_db.rollback(); }
void Transaction::commit()
{
    require(m_db.commit(), "Cannot commit Introvert settings to app.db. Check database access.");
    m_committed = true;
}

} // namespace introvert::sql
