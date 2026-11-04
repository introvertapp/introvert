#include "job_store.h"
#include "core/sqlite.h"
#include "core/queue_order.h"
#include "segments/identity.h"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlRecord>
#include <QTimeZone>
#include <QUuid>
#include <algorithm>
#include <chrono>
#include <cmath>

namespace introvert::detection {
namespace {
double now() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }
QString json(const QVariantMap &map) { return QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(map)).toJson(QJsonDocument::Compact)); }
QVariantList readRows(QSqlDatabase &db, const QString &statement, const QVariantList &values = {})
{
    auto query = sql::execute(db, statement, values); QVariantList result;
    const auto record = query.record();
    while (query.next()) { QVariantMap row; for (int i = 0; i < record.count(); ++i) row[record.fieldName(i)] = query.value(i); result.append(row); }
    return result;
}
const QString summaryColumns = "d.id,provider,show_id,title,season,kinds,status,current,total,message,error,result,created,updated,status_revision,q.position AS queueOrder,q.running AS queueRunning";
const QString summaryFrom = " FROM detection_jobs d LEFT JOIN queue_order q ON q.key='d:'||d.id ";
}
void DetectionJobStore::initialize() const
{
    sql::Connection connection(m_path); sql::Transaction transaction(connection.db);
    // Detection now analyzes media afresh for every job. Remove only the
    // retired derived-feature storage, preserving saved segments and history.
    sql::execute(connection.db, "DROP TABLE IF EXISTS detection_features");
    sql::execute(connection.db, "CREATE TABLE IF NOT EXISTS detection_jobs(id TEXT PRIMARY KEY,provider TEXT,show_id TEXT,title TEXT, season INTEGER,kinds TEXT,status TEXT,current INTEGER,total INTEGER,message TEXT,error TEXT, result TEXT,diagnostics TEXT,created REAL,updated REAL)");
    auto columns=sql::execute(connection.db,"PRAGMA table_info(detection_jobs)");bool hasRevision=false;
    while(columns.next())hasRevision|=columns.value(1).toString()=="status_revision";columns.finish();
    if(!hasRevision)sql::execute(connection.db,"ALTER TABLE detection_jobs ADD COLUMN status_revision INTEGER NOT NULL DEFAULT 0");
    queueOrder::initialize(connection.db);
    sql::execute(connection.db, "DELETE FROM queue_order WHERE key LIKE 'd:%'");
    sql::execute(connection.db, "UPDATE detection_jobs SET status='failed',message='Previous detection was interrupted',updated=? WHERE status IN ('queued','running')", {now()}); transaction.commit();
}
QVariantList DetectionJobStore::rows() const
{
    sql::Connection connection(m_path); return readRows(connection.db, "SELECT * FROM detection_jobs ORDER BY created DESC");
}
QVariantList DetectionJobStore::summaries() const
{
    // Selecting diagnostics here makes SQLite copy/sort potentially hundreds of
    // megabytes of historical evidence on every progress or navigation update.
    sql::Connection connection(m_path); return readRows(connection.db, "SELECT " + summaryColumns + summaryFrom + "ORDER BY created DESC");
}
QVariantMap DetectionJobStore::row(const QString &id) const
{
    sql::Connection connection(m_path); const auto rows = readRows(connection.db, "SELECT * FROM detection_jobs WHERE id=?", {id});
    return rows.isEmpty() ? QVariantMap{} : rows.first().toMap();
}
QPair<QString, bool> DetectionJobStore::create(const providers::ShowDetail &detail, int season, const QStringList &kinds) const
{
    const auto provider = providers::providerName(detail.show.identity.provider); sql::Connection connection(m_path); sql::Transaction transaction(connection.db);
    auto query = sql::execute(connection.db, "SELECT id FROM detection_jobs WHERE provider=? AND show_id=? AND season=? AND status IN ('queued','running') ORDER BY created DESC LIMIT 1", {provider, detail.show.identity.itemId, season});
    if (query.next()) { const auto id = query.value(0).toString(); query.finish(); transaction.commit(); return {id, false}; } query.finish();
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces); const auto time = now();
    sql::execute(connection.db, "INSERT INTO detection_jobs VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", {id, provider, detail.show.identity.itemId, detail.show.title, season,
        QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(kinds)).toJson(QJsonDocument::Compact)), "queued", 0, 1000, "Scan queued", "", "{}", "{}", time, time,segments::currentRevision(connection.db)});
    queueOrder::append(connection.db, "d:" + id); transaction.commit(); return {id, true};
}
void DetectionJobStore::progress(const QString &id, const QString &phase, int current, int total) const
{
    const QStringList phases{"loading media", "inspection", "planning", "extraction", "generation", "resolution", "refinement", "output", "saving results"};
    const auto index = phases.indexOf(phase); const double fraction = total > 0 ? std::clamp(double(current) / total, 0., 1.) : 0.;
    // Session components may report their own aggregate progress without exposing
    // internal stages to the host. Existing phase weights remain unchanged.
    const int units = phase == "analysis" ? std::min(999,int(fraction*1000.))
        : index < 0 ? 0 : std::min(999, int((double(index) + fraction) / phases.size() * 1000.)); sql::Connection connection(m_path); sql::Transaction transaction(connection.db);
    sql::execute(connection.db, "UPDATE detection_jobs SET status='running',current=MAX(current,?),total=?,message=?,updated=? WHERE id=? AND status IN ('queued','running')", {units, 1000, "Scanning: " + phase, now(), id});
    queueOrder::running(connection.db, "d:" + id, true); transaction.commit();
}
void DetectionJobStore::finish(const QString &id, const QString &status, const QString &message, const QVariantMap &result, const QVariantMap &diagnostics) const
{
    sql::Connection connection(m_path); sql::Transaction transaction(connection.db);
    sql::execute(connection.db, "UPDATE detection_jobs SET status=?,current=CASE WHEN ?='complete' THEN total ELSE current END,message=?,error=?,result=?,diagnostics=?,updated=? WHERE id=?",
        {status, status, message, status == "failed" ? message : QString(""), json(result), json(diagnostics), now(), id});
    queueOrder::remove(connection.db, "d:" + id); transaction.commit();
}
QVariantList DetectionJobStore::queue() const
{
    sql::Connection connection(m_path);
    return queue(readRows(connection.db, "SELECT " + summaryColumns + summaryFrom + "WHERE status IN ('queued','running') ORDER BY created DESC"));
}
QVariantList DetectionJobStore::queue(const QVariantList &rows)
{
    QVariantList result;
    for (const auto &value : rows) {
        const auto row = value.toMap(); const auto status = row.value("status").toString(); if (status != "queued" && status != "running") continue;
        const int season = row.value("season").toInt();
        result.append(QVariantMap{{"type", "Detection"}, {"endpoint", ""}, {"show", row.value("title")}, {"episode", "Season " + QString::number(season)}, {"code", QString("S%1").arg(season, 2, 10, QLatin1Char('0'))},
            {"date", QDateTime::fromMSecsSinceEpoch(qint64(std::floor(std::nearbyint(row.value("created").toDouble() * 1000000.) / 1000.)), QTimeZone::UTC).toString(Qt::ISODateWithMs)}, {"status", status == "running" || row.value("queueRunning").toBool() ? "Detecting" : "Queued"},
            {"showId", row.value("show_id")}, {"providerId", row.value("provider")}, {"jobId", row.value("id")}, {"progressCurrent", row.value("current")}, {"progressTotal", row.value("total")}, {"queueKey", "d:" + row.value("id").toString()},
            {"queueOrder", row.value("queueOrder")}, {"queueRunning", status == "running" || row.value("queueRunning").toBool()}});
    }
    return result;
}
} // namespace introvert::detection
