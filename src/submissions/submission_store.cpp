#include "submission_store.h"
#include "core/queue_order.h"
#include "segments/identity.h"
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonValue>
#include <QSqlRecord>
#include <QSet>
#include <QTimeZone>
#include <QUuid>
#include <algorithm>
#include <cmath>

namespace introvert::submissions {
namespace {
const QString pendingSql = "('WAITING','PROCESSING','RATE LIMITED')";
const QString activeSql = "('queued','running','rate_limited')";
const QStringList kinds{"recap", "intro", "credits", "preview"};

QList<QVariantMap> rows(QSqlDatabase &db, const QString &statement, const QVariantList &arguments = {})
{
    QList<QVariantMap> result;
    auto query = sql::execute(db, statement, arguments);
    while (query.next()) {
        QVariantMap row; const auto record = query.record();
        for (int index = 0; index < record.count(); ++index) row[record.fieldName(index)] = query.value(index);
        result.append(row);
    }
    return result;
}
bool exists(QSqlDatabase &db, const QString &statement, const QVariantList &arguments = {})
{
    auto query = sql::execute(db, statement, arguments); return query.next();
}
QString label(const QString &endpoint) { return endpoint == "introdb_app" ? QString("introdb.app") : QString("theintrodb.org"); }
QString title(QString text) { if (!text.isEmpty()) text[0] = text[0].toUpper(); return text; }
QString timestamp(double seconds)
{
    if (!std::isfinite(seconds) || seconds < -62135596800. || seconds >= 253402300800.)
        throw DatabaseError("Cannot format submission timestamp.");
    const auto milliseconds = std::floor(std::nearbyint(seconds * 1000000) / 1000);
    return QDateTime::fromMSecsSinceEpoch(qint64(milliseconds), QTimeZone::UTC).toString(Qt::ISODateWithMs);
}
double resumeTime(const std::optional<QString> &value, double now)
{
    auto date = QDateTime::fromString(value.value_or(QString()), Qt::ISODateWithMs);
    if (!date.isValid()) date = QDateTime::fromString(value.value_or(QString()), Qt::ISODate);
    return date.isValid() ? date.toMSecsSinceEpoch() / 1000. : now + 3600;
}
QVariant nullable(const QVariant &value)
{
    if (value.isNull()) return {};
    return value.metaType().id() == QMetaType::QString ? QVariant(sql::text(value.toString())) : value;
}
QMap<QString, QString> identifiers(const QVariantMap &record)
{
    QMap<QString, QString> result;
    const auto encoded = record.value("canonical");
    const auto canonical = encoded.metaType().id() == QMetaType::QString
        ? QJsonValue::fromJson(encoded.toString().toUtf8()).toVariant().toStringList() : encoded.toStringList();
    for (const auto &key : canonical) {
        const auto colon = key.indexOf(':'); const auto kind = key.left(colon);
        if (QStringList{"tmdb", "tvdb", "imdb"}.contains(kind)) result[kind] = key.mid(colon + 1);
    }
    for (const auto &kind : {QString("tmdb"), QString("tvdb"), QString("imdb")})
        if (!record.value(kind).toString().isEmpty()) result[kind] = record.value(kind).toString();
    return result;
}
bool compatible(const QVariantMap &left, const QVariantMap &right, bool *shared = nullptr)
{
    const auto a = identifiers(left), b = identifiers(right); bool match = false;
    for (auto i = a.cbegin(); i != a.cend(); ++i) if (b.contains(i.key())) {
        if (b.value(i.key()) != i.value()) return false;
        match = true;
    }
    if (shared) *shared = match;
    return true;
}
}

SubmissionStore::SubmissionStore(QString path, Clock clock) : m_path(std::move(path)), m_clock(std::move(clock))
{
    if (!m_clock) m_clock = [] { return QDateTime::currentMSecsSinceEpoch() / 1000.; };
}

void SubmissionStore::initialize() const
{
    if (!QDir().mkpath(QFileInfo(m_path).absolutePath())) throw DatabaseError("Cannot access INTROvert app.db.");
    sql::Connection connection(m_path); auto &db = connection.db; sql::Transaction transaction(db);
    sql::execute(db, "CREATE TABLE IF NOT EXISTS submit_jobs("
        "id TEXT PRIMARY KEY,endpoint TEXT NOT NULL,provider TEXT NOT NULL,show_id TEXT NOT NULL,"
        "title TEXT NOT NULL,imdb TEXT,tmdb TEXT,canonical TEXT NOT NULL,season INTEGER NOT NULL,"
        "status TEXT NOT NULL,created REAL NOT NULL,updated REAL NOT NULL,next_at REAL NOT NULL DEFAULT 0)");
    sql::execute(db, "CREATE TABLE IF NOT EXISTS submit_items("
        "id INTEGER PRIMARY KEY,job_id TEXT NOT NULL REFERENCES submit_jobs(id),"
        "episode_id TEXT NOT NULL,title TEXT NOT NULL,season INTEGER NOT NULL,number INTEGER NOT NULL,"
        "kind TEXT NOT NULL,start REAL NOT NULL,end REAL,duration_ms INTEGER,"
        "status TEXT NOT NULL DEFAULT 'WAITING',comparison TEXT,error TEXT NOT NULL DEFAULT '',"
        "http_status INTEGER,created REAL NOT NULL,updated REAL NOT NULL,revision INTEGER NOT NULL DEFAULT 1,"
        "UNIQUE(job_id,episode_id,kind))");
    sql::execute(db, "CREATE TABLE IF NOT EXISTS submit_api_rate_limits("
        "endpoint TEXT PRIMARY KEY,resume_at REAL NOT NULL,reset_at TEXT,error TEXT NOT NULL)");
    sql::execute(db, "CREATE TABLE IF NOT EXISTS local_submissions("
        "item_id INTEGER PRIMARY KEY REFERENCES submit_items(id),endpoint TEXT NOT NULL,"
        "canonical TEXT NOT NULL,season INTEGER NOT NULL,episode INTEGER NOT NULL,kind TEXT NOT NULL,"
        "start REAL NOT NULL,end REAL,submitted REAL NOT NULL)");
    sql::execute(db, "CREATE INDEX IF NOT EXISTS submit_items_pending ON submit_items(job_id,status,id)");
    auto columns=sql::execute(db,"PRAGMA table_info(submit_items)");bool hasRevision=false;
    while(columns.next())hasRevision|=columns.value(1).toString()=="status_revision";columns.finish();
    if(!hasRevision)sql::execute(db,"ALTER TABLE submit_items ADD COLUMN status_revision INTEGER NOT NULL DEFAULT 0");
    auto fields=sql::execute(db,"PRAGMA table_info(submit_items)");bool hasAddress=false;
    while(fields.next())hasAddress|=fields.value(1).toString()=="address";fields.finish();
    if(!hasAddress)sql::execute(db,"ALTER TABLE submit_items ADD COLUMN address TEXT NOT NULL DEFAULT '{}'");
    queueOrder::initialize(db);
    // Older jobs retain unknown scope. Preserve their work/history; never infer
    // a server from whichever connection happens to be configured at startup.
    sql::execute(db, "CREATE TABLE IF NOT EXISTS submit_job_scopes(job_id TEXT PRIMARY KEY,scope TEXT NOT NULL)");
    sql::execute(db, "CREATE INDEX IF NOT EXISTS submit_jobs_status ON submit_jobs(status,created)");
    sql::execute(db, "CREATE TABLE IF NOT EXISTS submission_state(id INTEGER PRIMARY KEY CHECK(id=1),revision INTEGER NOT NULL)");
    sql::execute(db, "INSERT OR IGNORE INTO submission_state VALUES(1,0)");
    // A lightweight change token also observes other connections/processes.
    // Detector progress and unrelated settings must not invalidate history.
    for (const auto &table : {"submit_jobs", "submit_items", "submit_api_rate_limits", "submit_job_scopes", "queue_order", "endpoint_settings", "credentials"}) {
        auto present = sql::execute(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?", {table});
        if (!present.next()) continue; present.finish();
        for (const auto &operation : {QString("INSERT"), QString("UPDATE"), QString("DELETE")}) {
            const auto record = operation == "DELETE" ? QString("OLD") : QString("NEW");
            const auto condition = QString(table) == "queue_order" ? " WHEN " + record + ".key LIKE 's:%'"
                : QString(table) == "credentials" ? " WHEN " + record + ".account IN ('endpoints:introdb_app','endpoints:theintrodb_org')" : QString();
            sql::execute(db, "CREATE TRIGGER IF NOT EXISTS submission_changed_" + QString(table) + '_' + operation
                + " AFTER " + operation + " ON " + table + condition + " BEGIN UPDATE submission_state SET revision=revision+1 WHERE id=1; END");
        }
    }
    // Backfill existing installations without changing item identities/history.
    for (const auto &row : rows(db, "SELECT i.job_id,i.episode_id FROM submit_items i JOIN submit_jobs j ON j.id=i.job_id "
             "WHERE j.status IN " + activeSql + " AND i.status IN " + pendingSql + " GROUP BY i.job_id,i.episode_id ORDER BY MIN(i.id)"))
        queueOrder::append(db, "s:" + row.value("job_id").toString() + ':' + row.value("episode_id").toString());
    transaction.commit();
}

bool SubmissionStore::moveQueueItem(const QString &key, const QString &target, bool after, const QStringList &order) const
{
    sql::Connection connection(m_path); sql::Transaction transaction(connection.db);
    const bool moved = queueOrder::move(connection.db, key, target, after, order);
    transaction.commit(); return moved;
}

QString SubmissionStore::enqueue(const QVariantMap &plan) const
{
    const auto now = m_clock();
    sql::Connection connection(m_path); auto &db = connection.db; sql::Transaction transaction(db);
    const auto candidates = rows(db, "SELECT j.*,COALESCE(s.scope,'') AS scope FROM submit_jobs j LEFT JOIN submit_job_scopes s ON s.job_id=j.id WHERE endpoint=? AND season=? AND status IN " + activeSql
        + " ORDER BY created DESC", {plan.value("endpoint"), plan.value("season")});
    QString id;
    for (const auto &candidate : candidates) {
        bool shared = false; if (!compatible(candidate, plan, &shared)) continue;
        const bool sameSource = candidate.value("scope").toString() == plan.value("scope").toString()
            && candidate.value("provider") == plan.value("provider") && candidate.value("show_id") == plan.value("show_id");
        if (sameSource || shared) { id = candidate.value("id").toString(); break; }
    }
    const bool creating = id.isEmpty();
    if (creating) id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto statusRevision=segments::currentRevision(db);
    if (creating) {
        const auto canonical = QString::fromUtf8(QJsonValue::fromVariant(plan.value("canonical")).toJson(QJsonValue::JsonFormat::Compact));
        sql::execute(db, "INSERT INTO submit_jobs VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)",
            {id, plan.value("endpoint"), plan.value("provider"), plan.value("show_id"), sql::text(plan.value("show").toString()),
             nullable(plan.value("imdb")), nullable(plan.value("tmdb")), canonical, plan.value("season"), "queued", now, now, 0});
        sql::execute(db, "INSERT INTO submit_job_scopes VALUES(?,?)", {id, sql::text(plan.value("scope").toString())});
    }
    for (const auto &value : plan.value("items").toList()) {
        const auto item = value.toMap();
        QVariantMap address;
        for (const auto &name : {"tvdb_id","tmdb_id","imdb_id","numbering_source","mapping","tvdb_episode_id",
                                 "target_season","target_episode","episode_name","resolved_title"})
            if(item.contains(name)) address[name]=item[name];
        const auto encoded=QString::fromUtf8(QJsonValue::fromVariant(address).toJson(QJsonValue::JsonFormat::Compact));
        sql::execute(db, "INSERT INTO submit_items(job_id,episode_id,title,season,number,kind,start,end,duration_ms,created,updated,status_revision,address) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(job_id,episode_id,kind) DO UPDATE SET "
            "title=excluded.title,start=excluded.start,end=excluded.end,duration_ms=excluded.duration_ms,"
            "status=CASE WHEN submit_items.start IS excluded.start AND submit_items.end IS excluded.end "
            "AND submit_items.address IS excluded.address AND submit_items.duration_ms IS excluded.duration_ms "
            "AND submit_items.status IN ('OK','DUPLICATE') THEN submit_items.status ELSE 'WAITING' END,"
            "error='',comparison=NULL,updated=excluded.updated,revision=submit_items.revision+1,status_revision=excluded.status_revision,address=excluded.address",
            {id, item.value("episode_id"), sql::text(item.value("title").toString()), item.value("season"), item.value("episode"),
             item.value("segment_type"), item.value("start_sec"), nullable(item.value("end_sec")), nullable(item.value("video_duration_ms")), now, now,statusRevision,encoded});
    }
    for (const auto &row : rows(db, "SELECT episode_id FROM submit_items WHERE job_id=? AND status IN " + pendingSql + " GROUP BY episode_id ORDER BY MIN(id)", {id}))
        queueOrder::append(db, "s:" + id + ':' + row.value("episode_id").toString());
    sql::execute(db, "UPDATE submit_jobs SET updated=? WHERE id=?", {now, id});
    transaction.commit(); return id;
}

std::optional<SubmissionClaim> SubmissionStore::nextItem() const
{
    const auto now = m_clock();
    sql::Connection connection(m_path); auto &db = connection.db; sql::Transaction transaction(db);
    sql::execute(db, "DELETE FROM submit_api_rate_limits WHERE resume_at<=?", {now});
    const auto jobs = rows(db, "SELECT * FROM submit_jobs WHERE status IN " + activeSql + " ORDER BY created");
    for (const auto &job : jobs) if (job.value("next_at").toDouble() > now && job.value("status") == "running") {
        transaction.commit(); return {};
    }
    // A displayed submission row is one episode (all selected segment kinds).
    // Keep its in-flight kinds together; other episodes follow the saved order.
    sql::execute(db, "UPDATE submit_jobs SET status='rate_limited' WHERE status IN " + activeSql +
        " AND status!='rate_limited' AND endpoint IN (SELECT endpoint FROM submit_api_rate_limits)");
    const auto candidates = rows(db, "SELECT i.* FROM submit_items i JOIN submit_jobs j ON j.id=i.job_id "
        "JOIN queue_order q ON q.key='s:'||j.id||':'||i.episode_id WHERE j.status IN " + activeSql +
        " AND i.status IN " + pendingSql + " AND j.next_at<=? AND NOT EXISTS "
        "(SELECT 1 FROM submit_api_rate_limits l WHERE l.endpoint=j.endpoint) ORDER BY q.running DESC,q.position,i.id LIMIT 1", {now});
    for (const auto &item : candidates) {
        const auto found = std::find_if(jobs.cbegin(), jobs.cend(), [&](const auto &job) { return job.value("id") == item.value("job_id"); });
        if (found == jobs.cend()) continue;
        const auto &job = *found;
        sql::execute(db, "UPDATE submit_jobs SET status='running' WHERE id=?", {job.value("id")});
        sql::execute(db, "UPDATE submit_items SET status='PROCESSING' WHERE id=?", {item.value("id")});
        queueOrder::running(db, "s:" + job.value("id").toString() + ':' + item.value("episode_id").toString(), true);
        transaction.commit(); return SubmissionClaim{job, item};
    }
    transaction.commit(); return {};
}

void SubmissionStore::finish(const SubmissionClaim &claim, const SubmissionCompletion &completion) const
{
    const auto now = m_clock(); const auto &job = claim.job; const auto &item = claim.item;
    sql::Connection connection(m_path); auto &db = connection.db; sql::Transaction transaction(db);
    const auto update = sql::execute(db, "UPDATE submit_items SET status=?,comparison=?,error=?,http_status=?,updated=? WHERE id=? AND revision=?",
        {completion.status, completion.comparison ? QVariant(*completion.comparison) : QVariant(), sql::text(completion.error),
         completion.httpStatus ? QVariant(*completion.httpStatus) : QVariant(), now, item.value("id"), item.value("revision")});
    const bool updated = update.numRowsAffected() > 0;
    if (completion.limit) {
        const auto &limit = *completion.limit;
        sql::execute(db, "INSERT OR REPLACE INTO submit_api_rate_limits VALUES(?,?,?,?)",
            {job.value("endpoint"), resumeTime(limit.resumeAt, now), limit.resetAt ? QVariant(*limit.resetAt) : QVariant(), sql::text(limit.error)});
        sql::execute(db, "UPDATE submit_jobs SET status='rate_limited',updated=? WHERE endpoint=? AND status IN " + activeSql, {now, job.value("endpoint")});
    } else {
        const bool more = exists(db, "SELECT 1 FROM submit_items WHERE job_id=? AND episode_id=? AND status IN " + pendingSql + " LIMIT 1",
                                 {job.value("id"), item.value("episode_id")});
        const bool remaining = exists(db, "SELECT 1 FROM submit_items WHERE job_id=? AND status IN " + pendingSql + " LIMIT 1", {job.value("id")});
        const auto delay = completion.settings && !more && remaining ? std::max(0, completion.settings->value("api_delay_seconds").toInt()) : 0;
        sql::execute(db, "UPDATE submit_jobs SET next_at=?,updated=? WHERE id=?", {now + delay, now, job.value("id")});
    }
    const auto key = "s:" + job.value("id").toString() + ':' + item.value("episode_id").toString();
    if (!exists(db, "SELECT 1 FROM submit_items WHERE job_id=? AND episode_id=? AND status IN " + pendingSql,
                {job.value("id"), item.value("episode_id")})) queueOrder::remove(db, key);
    else if (completion.limit) queueOrder::running(db, key, false);
    if (updated && completion.status == "OK" && completion.settings && completion.settings->value("save_submissions_local_db").toBool())
        sql::execute(db, "INSERT OR REPLACE INTO local_submissions VALUES(?,?,?,?,?,?,?,?,?)",
            {item.value("id"), job.value("endpoint"), job.value("canonical"), item.value("season"), item.value("number"), item.value("kind"),
             item.value("start"), nullable(item.value("end")), now});
    if (!exists(db, "SELECT 1 FROM submit_items WHERE job_id=? AND status IN " + pendingSql + " LIMIT 1", {job.value("id")}))
        sql::execute(db, "UPDATE submit_jobs SET status='complete',updated=? WHERE id=?", {now, job.value("id")});
    transaction.commit();
}

bool SubmissionStore::processOne(const SettingsStore &settings, const providers::Cancellation &cancelled,
                                 const std::function<void()> &progress) const
{
    const auto selected = nextItem();
    if (!selected) return false;
    const auto &job = selected->job; const auto &item = selected->item;
    if (progress) progress();
    const auto client = EndpointClient::fromStore(job.value("endpoint").toString(), settings, m_clock);
    QVariantMap values{{"imdb_id", nullable(job.value("imdb"))}, {"tmdb_id", nullable(job.value("tmdb"))},
        {"season", item.value("season")}, {"episode", item.value("number")}, {"segment_type", item.value("kind")},
        {"start_sec", item.value("start")}, {"end_sec", nullable(item.value("end"))}, {"video_duration_ms", nullable(item.value("duration_ms"))}};
    const auto address=QJsonValue::fromJson(item.value("address").toString().toUtf8()).toObject().toVariantMap();
    for(auto i=address.begin();i!=address.end();++i) values[i.key()]=i.value();
    SubmissionCompletion completion; completion.settings = client.settings();
    try {
        if(client.endpoint()=="theintrodb_org" && address.isEmpty()) throw SubmissionError("Review this queued item again to verify its v4 episode identity.");
        client.validate(values);
    }
    catch (const SubmissionError &error) {
        completion.status = "FAIL"; completion.error = QString::fromUtf8(error.what()); finish(*selected, completion); return true;
    }
    const auto lookup = client.lookup(values, cancelled);
    if (cancelled && cancelled->load()) return false;
    if(lookup.failure) {
        completion.status=lookup.failure->result;completion.error=lookup.failure->error;completion.httpStatus=lookup.failure->httpStatus;
        if(completion.status=="RATE LIMITED")completion.limit=*lookup.failure;
        finish(*selected,completion);return true;
    }
    completion.comparison = comparison(lookup, item.value("start").toDouble(), item.value("end").isNull()
        ? std::nullopt : std::optional<double>(item.value("end").toDouble()), client.endpoint() == "theintrodb_org");
    if (*completion.comparison == "DUPLICATE") completion.status = "DUPLICATE";
    else {
        const auto result = client.submit(values, cancelled);
        if (cancelled && cancelled->load()) return false;
        completion.status = result.result; completion.error = result.error; completion.httpStatus = result.httpStatus;
        if (result.result == "RATE LIMITED") completion.limit = result;
    }
    finish(*selected, completion); return true;
}

QVariantMap SubmissionStore::episodeStatuses(bool includeActivity, const QString &scope, const QStringList &canonical) const
{
    sql::Connection connection(m_path);
    const auto records = rows(connection.db, "SELECT j.provider,j.show_id,i.episode_id,i.kind,i.status,i.comparison,i.error,i.http_status,j.endpoint,i.start,i.end,i.updated,i.status_revision,i.id,i.revision "
        ",j.id AS source_job,j.canonical,j.tmdb,j.imdb,COALESCE(s.scope,'') AS scope FROM submit_items i JOIN submit_jobs j ON j.id=i.job_id "
        "LEFT JOIN submit_job_scopes s ON s.job_id=j.id ORDER BY i.updated DESC,i.id DESC");
    QVariantMap result;
    QMap<QString, bool> visibleJobs;
    for (const auto &item : records) {
        if (!scope.isEmpty() || !canonical.isEmpty()) {
            const auto job = item.value("source_job").toString();
            if (!visibleJobs.contains(job)) {
                bool shared = false;
                visibleJobs[job] = compatible(item, {{"canonical", canonical}}, &shared)
                    && (scope.isEmpty() || item.value("scope").toString() == scope || shared);
            }
            if (!visibleJobs.value(job)) continue;
        }
        const auto key = item.value("provider").toString() + ':' + item.value("show_id").toString() + ':' + item.value("episode_id").toString();
        auto state = result.value(key).toMap();
        auto segments = state.value("segments").toMap(), badges = state.value("badges").toMap();
        const auto kind = item.value("kind").toString();
        if (!segments.contains(kind)) {
            QVariantMap current{{"kind", kind}, {"result", item.value("status")},
                {"comparison", item.value("comparison").isNull() ? QString("") : item.value("comparison").toString()},
                {"error", item.value("error")}, {"httpStatus", nullable(item.value("http_status"))}};
            if(item.value("status_revision").toLongLong()>0)current["statusRevision"]=item.value("status_revision");
            if(includeActivity)current["activity"]=QString("%1:%2:%3").arg(item.value("id").toLongLong()).arg(item.value("revision").toLongLong())
                .arg(item.value("updated").toDouble(),0,'g',17);
            segments[kind]=current;
        }
        if (item.value("status") == "OK" || item.value("status") == "DUPLICATE") {
            auto values = badges.value(kind).toList();
            QVariantMap badge{{"endpoint", item.value("endpoint")}, {"start", item.value("start")}, {"end", nullable(item.value("end"))}};
            if(item.value("status_revision").toLongLong()>0)badge["statusRevision"]=item.value("status_revision");
            if (!values.contains(badge)) values.append(badge);
            badges[kind] = values;
        }
        state["segments"] = segments; state["badges"] = badges; result[key] = state;
    }
    return result;
}

SubmissionSnapshot SubmissionStore::snapshot() const
{
    const auto now = m_clock();
    sql::Connection connection(m_path); auto &db = connection.db;
    // Keep related reads consistent with concurrent append transactions.
    sql::execute(db, "BEGIN");
    QMap<QString, QVariantMap> jobs;
    for (const auto &row : rows(db, "SELECT j.*,COALESCE(s.scope,'') AS scope FROM submit_jobs j LEFT JOIN submit_job_scopes s ON s.job_id=j.id ORDER BY created")) jobs[row.value("id").toString()] = row;
    const auto items = rows(db, "SELECT i.*,q.key AS queueKey,q.position AS queueOrder,q.running AS queueRunning FROM submit_items i "
        "LEFT JOIN queue_order q ON q.key='s:'||i.job_id||':'||i.episode_id ORDER BY i.id");
    QMap<QString, double> limits;
    for (const auto &row : rows(db, "SELECT * FROM submit_api_rate_limits WHERE resume_at>?", {now})) limits[row.value("endpoint").toString()] = row.value("resume_at").toDouble();
    sql::execute(db, "COMMIT");
    SubmissionSnapshot result;
    struct Entry { QVariantMap item, row; };
    QList<QList<Entry>> groups; QMap<QPair<QString, QString>, qsizetype> indexes;
    const QStringList pending{"WAITING", "PROCESSING", "RATE LIMITED"};
    for (const auto &item : items) {
        const auto job = jobs.value(item.value("job_id").toString());
        const auto status = item.value("status").toString(), endpoint = job.value("endpoint").toString();
        QString text;
        if (job.value("status") != "complete" && pending.contains(status) && limits.contains(endpoint)) {
            const auto seconds = qint64(std::max(0., limits.value(endpoint) - now));
            text = QString("Rate limited %1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QChar('0'));
        } else if (status == "WAITING" || status == "RATE LIMITED") text = "Queued";
        else if (status == "PROCESSING") text = "Submitting";
        else if (status == "FAIL") text = "Fail";
        else if (status == "DUPLICATE" || status == "SKIP") text = status.left(1) + status.mid(1).toLower();
        else text = status;
        const auto tmdb = job.value("tmdb"), imdb = job.value("imdb");
        QVariantMap row{{"type", title(item.value("kind").toString())}, {"endpoint", label(endpoint)}, {"show", job.value("title")},
            {"episode", item.value("title")}, {"code", QString("S%1E%2").arg(item.value("season").toLongLong(), 2, 10, QChar('0')).arg(item.value("number").toLongLong(), 2, 10, QChar('0'))},
            {"date", timestamp(item.value("created").toDouble())}, {"status", text}, {"providerId", job.value("provider")}, {"showId", job.value("show_id")},
            {"jobId", job.value("id")}, {"error", item.value("error")}, {"canonical", !tmdb.isNull() && !tmdb.toString().isEmpty() ? tmdb
                : !imdb.isNull() && !imdb.toString().isEmpty() ? imdb : QVariant(job.value("provider").toString() + ':' + job.value("show_id").toString())}};
        if (!job.value("scope").toString().isEmpty()) row["scope"] = job.value("scope");
        if (job.value("status") == "complete") result.history.append(row);
        else {
            const auto key = qMakePair(job.value("id").toString(), item.value("episode_id").toString());
            if (!indexes.contains(key)) { indexes[key] = groups.size(); groups.append(QList<Entry>()); }
            groups[indexes.value(key)].append({item, row});
        }
    }
    for (const auto &group : groups) {
        if (std::all_of(group.cbegin(), group.cend(), [](const Entry &entry) {
                return QStringList{"OK", "DUPLICATE", "SKIP"}.contains(entry.item.value("status").toString()); })) continue;
        auto selected = group.first().row;
        int completed = 0;
        bool found = false;
        for (const auto &entry : group) {
            const auto state = entry.item.value("status").toString();
            if (!pending.contains(state)) ++completed;
            if (!found && (pending.contains(state) || state == "FAIL")) { selected = entry.row; found = true; }
        }
        selected["queueKey"] = group.first().item.value("queueKey");
        selected["queueOrder"] = group.first().item.value("queueOrder").isNull() ? group.first().item.value("id") : group.first().item.value("queueOrder");
        selected["queueRunning"] = group.first().item.value("queueRunning").toBool();
        selected["type"] = "Submission"; selected["progressCurrent"] = completed; selected["progressTotal"] = group.size();
        result.queue.append(selected);
    }
    std::stable_sort(result.queue.begin(), result.queue.end(), [](const auto &a, const auto &b) {
        const auto left = a.toMap(), right = b.toMap();
        if (left.value("queueRunning").toBool() != right.value("queueRunning").toBool()) return left.value("queueRunning").toBool();
        return left.value("queueOrder").toLongLong() < right.value("queueOrder").toLongLong();
    });
    QSet<QString> shows; QSet<QPair<QString, QString>> episodes;
    result.statistics = {{"Shows", 0}, {"Episodes", 0}, {"IntroDB", 0}, {"TheIntroDB", 0}};
    for (const auto &kind : kinds) result.statistics[title(kind)] = 0;
    for (const auto &value : result.history) {
        const auto row = value.toMap();
        const auto show = (row.value("showId").toString().isEmpty() ? row.value("show").toString() : row.value("showId").toString()).trimmed().toLower();
        shows.insert(show); episodes.insert(qMakePair(show, row.value("code").toString()));
        const auto type = row.value("type").toString();
        if (kinds.contains(type.toLower())) result.statistics[type] = result.statistics.value(type).toInt() + 1;
        const auto endpoint = row.value("endpoint") == "introdb.app" ? QString("IntroDB") : QString("TheIntroDB");
        result.statistics[endpoint] = result.statistics.value(endpoint).toInt() + 1;
    }
    result.statistics["Shows"] = shows.size(); result.statistics["Episodes"] = episodes.size();
    std::reverse(result.history.begin(), result.history.end());
    return result;
}

} // namespace introvert::submissions
