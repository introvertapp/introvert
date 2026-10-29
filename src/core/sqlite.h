#pragma once

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QVariant>
#include <stdexcept>

namespace introvert {

class DatabaseError final : public std::runtime_error {
public:
    explicit DatabaseError(const char *message) : std::runtime_error(message) {}
};

namespace sql {

QSqlQuery execute(QSqlDatabase &db, const QString &statement, const QVariantList &values = {});
QString text(const QString &value);
void restrictPermissions(const QString &path);

// Each instance is constructed, used and destroyed on its calling thread.
class Connection final {
public:
    explicit Connection(const QString &path);
    ~Connection();
    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;
private:
    void release();
    QString m_name;
public:
    QSqlDatabase db;
};

class Transaction final {
public:
    explicit Transaction(QSqlDatabase &db);
    ~Transaction();
    Transaction(const Transaction &) = delete;
    Transaction &operator=(const Transaction &) = delete;
    void commit();
private:
    QSqlDatabase &m_db;
    bool m_committed = false;
};

} // namespace sql
} // namespace introvert
