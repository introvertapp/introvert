#include "queue_order.h"

namespace introvert::queueOrder {

void initialize(QSqlDatabase &db)
{
    sql::execute(db, "CREATE TABLE IF NOT EXISTS queue_order(key TEXT PRIMARY KEY,position INTEGER NOT NULL,running INTEGER NOT NULL DEFAULT 0)");
}

void append(QSqlDatabase &db, const QString &key)
{
    sql::execute(db, "INSERT OR IGNORE INTO queue_order(key,position) SELECT ?,COALESCE(MAX(position),0)+1 FROM queue_order", {key});
}

void remove(QSqlDatabase &db, const QString &key)
{
    sql::execute(db, "DELETE FROM queue_order WHERE key=?", {key});
}

void running(QSqlDatabase &db, const QString &key, bool value)
{
    sql::execute(db, "UPDATE queue_order SET running=? WHERE key=?", {value, key});
}

bool move(QSqlDatabase &db, const QString &key, const QString &target, bool after, const QStringList &order)
{
    if (key == target) return false;
    QStringList keys;
    auto query = sql::execute(db, "SELECT key FROM queue_order WHERE running=0 ORDER BY position,key");
    while (query.next()) keys.append(query.value(0).toString());
    query.finish();
    if (!keys.contains(key) || !keys.contains(target)) return false;
    if (order.isEmpty()) {
        keys.removeOne(key);
        keys.insert(keys.indexOf(target) + (after ? 1 : 0), key);
    } else {
        // A drag from a column-sorted/search view preserves its visible order.
        // Unmentioned appends keep their slots; newly running rows stay pinned.
        QStringList selected;
        for (const auto &entry : order) if (keys.contains(entry) && !selected.contains(entry)) selected.append(entry);
        if (!selected.contains(key) || !selected.contains(target)) return false;
        int next = 0;
        for (auto &entry : keys) if (selected.contains(entry)) entry = selected[next++];
    }
    for (int i = 0; i < keys.size(); ++i)
        sql::execute(db, "UPDATE queue_order SET position=? WHERE key=?", {i + 1, keys[i]});
    return true;
}

QString claim(QSqlDatabase &db, const QStringList &keys)
{
    auto query = sql::execute(db, "SELECT key FROM queue_order ORDER BY running DESC,position,key");
    QString selected;
    while (query.next()) if (keys.contains(query.value(0).toString())) { selected = query.value(0).toString(); break; }
    query.finish();
    if (!selected.isEmpty()) running(db, selected, true);
    return selected;
}

} // namespace introvert::queueOrder
