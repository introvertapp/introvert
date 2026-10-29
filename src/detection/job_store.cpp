#include "job_store.h"
#include "core/sqlite.h"
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
}
void DetectionJobStore::initialize() const
{
    sql::Connection connection(m_path); sql::Transaction transaction(connection.db);
    // Detection now analyzes media afresh for every job. Remove only the
    // retired derived-feature storage, preserving saved segments and history.
    sql::execute(connection.db, "DROP TABLE IF EXISTS detection_features");
    sql::execute(connection.db, "CREATE TABLE IF NOT EXISTS detection_jobs(id TEXT PRIMARY KEY,provider TEXT,show_id TEXT,title TEXT, season INTEGER,kinds TEXT,status TEXT,current INTEGER,total INTEGER,message TEXT,error TEXT, result TEXT,diagnostics TEXT,created REAL,updated REAL)");
    sql::execute(connection.db, "UPDATE detection_jobs SET status='failed',message='Previous detection was interrupted',updated=? WHERE status IN ('queued','running')", {now()}); transaction.commit();
}
QVariantList DetectionJobStore::rows() const
{
    sql::Connection connection(m_path); auto query = sql::execute(connection.db, "SELECT * FROM detection_jobs ORDER BY created DESC"); QVariantList result;
    while (query.next()) { QVariantMap row; const auto record = query.record(); for (int i = 0; i < record.count(); ++i) row[record.fieldName(i)] = query.value(i); result.append(row); } return result;
}
QPair<QString, bool> DetectionJobStore::create(const providers::ShowDetail &detail, int season, const QStringList &kinds) const
{
    const auto provider = providers::providerName(detail.show.identity.provider); sql::Connection connection(m_path); sql::Transaction transaction(connection.db);
    auto query = sql::execute(connection.db, "SELECT id FROM detection_jobs WHERE provider=? AND show_id=? AND season=? AND status IN ('queued','running') ORDER BY created DESC LIMIT 1", {provider, detail.show.identity.itemId, season});
    if (query.next()) { const auto id = query.value(0).toString(); query.finish(); transaction.commit(); return {id, false}; } query.finish();
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces); const auto time = now();
    sql::execute(connection.db, "INSERT INTO detection_jobs VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", {id, provider, detail.show.identity.itemId, detail.show.title, season,
        QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(kinds)).toJson(QJsonDocument::Compact)), "queued", 0, 1000, "Scan queued", "", "{}", "{}", time, time}); transaction.commit(); return {id, true};
}
void DetectionJobStore::progress(const QString &id, const QString &phase, int current, int total) const
{
    const QStringList phases{"loading media", "inspection", "planning", "extraction", "generation", "resolution", "refinement", "output", "saving results"};
    const auto index = phases.indexOf(phase); const double fraction = total > 0 ? std::clamp(double(current) / total, 0., 1.) : 0.;
    const int units = index < 0 ? 0 : std::min(999, int((double(index) + fraction) / phases.size() * 1000.)); sql::Connection connection(m_path);
    sql::execute(connection.db, "UPDATE detection_jobs SET status='running',current=MAX(current,?),total=?,message=?,updated=? WHERE id=? AND status IN ('queued','running')", {units, 1000, "Scanning: " + phase, now(), id});
}
void DetectionJobStore::finish(const QString &id, const QString &status, const QString &message, const QVariantMap &result, const QVariantMap &diagnostics) const
{
    sql::Connection connection(m_path);
    sql::execute(connection.db, "UPDATE detection_jobs SET status=?,current=CASE WHEN ?='complete' THEN total ELSE current END,message=?,error=?,result=?,diagnostics=?,updated=? WHERE id=?",
        {status, status, message, status == "failed" ? message : QString(""), json(result), json(diagnostics), now(), id});
}
QVariantList DetectionJobStore::queue() const
{
    QVariantList result;
    for (const auto &value : rows()) {
        const auto row = value.toMap(); const auto status = row.value("status").toString(); if (status != "queued" && status != "running") continue;
        const int season = row.value("season").toInt();
        result.append(QVariantMap{{"type", "Detection"}, {"endpoint", ""}, {"show", row.value("title")}, {"episode", "Season " + QString::number(season)}, {"code", QString("S%1").arg(season, 2, 10, QLatin1Char('0'))},
            {"date", QDateTime::fromMSecsSinceEpoch(qint64(std::floor(std::nearbyint(row.value("created").toDouble() * 1000000.) / 1000.)), QTimeZone::UTC).toString(Qt::ISODateWithMs)}, {"status", status == "running" ? "Detecting" : "Queued"},
            {"showId", row.value("show_id")}, {"providerId", row.value("provider")}, {"jobId", row.value("id")}, {"progressCurrent", row.value("current")}, {"progressTotal", row.value("total")}});
    }
    return result;
}
} // namespace introvert::detection
