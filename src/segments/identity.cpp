#include "identity.h"
#include "core/sqlite.h"

#include <QRegularExpression>

namespace introvert::segments {

QStringList showKeys(const providers::ShowSummary &show)
{
    QStringList result;
    const auto &ids = show.externalIds;
    for (const auto &entry : {qMakePair(QStringLiteral("tmdb"), ids.tmdb), qMakePair(QStringLiteral("tvdb"), ids.tvdb),
                              qMakePair(QStringLiteral("imdb"), ids.imdb)})
        if (entry.second && !entry.second->trimmed().isEmpty()) result.append(entry.first + ':' + entry.second->trimmed());
    const auto external = !result.isEmpty();
    if (show.identity.provider == providers::ProviderId::Local) result.append("local:" + show.identity.itemId);
    if (external) return result;
    QString title;
    for (const auto c : show.title.toLower().normalized(QString::NormalizationForm_KD))
        if (c.unicode() < 0x0300 || c.unicode() > 0x036f) title.append(c);
    title.replace('&', " and ");
    title.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), "-");
    while (title.startsWith('-')) title.remove(0, 1);
    while (title.endsWith('-')) title.chop(1);
    if (!title.isEmpty()) result.append("title:" + title + (show.year && *show.year ? ':' + QString::number(*show.year) : QString()));
    return result;
}

void initializeTables(QSqlDatabase &db)
{
    auto version = sql::execute(db, "PRAGMA user_version"); version.next();
    const auto value = version.value(0).toInt(); version.finish();
    if (value < 0 || value > 2) throw DatabaseError("Unsupported Introvert segment database version");
    sql::execute(db, "CREATE TABLE IF NOT EXISTS segments (show_key TEXT NOT NULL,season INTEGER NOT NULL,episode INTEGER NOT NULL,"
        "kind TEXT NOT NULL,start REAL,end REAL,deleted INTEGER NOT NULL DEFAULT 0,source TEXT NOT NULL,provider TEXT,provider_episode TEXT,"
        "updated TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,revision INTEGER NOT NULL DEFAULT 0,PRIMARY KEY(show_key,season,episode,kind))");
    sql::execute(db, "CREATE TABLE IF NOT EXISTS episode_media(provider TEXT,episode_id TEXT,duration_ms INTEGER NOT NULL,PRIMARY KEY(provider,episode_id))");
    sql::execute(db, "CREATE TABLE IF NOT EXISTS preferences (name TEXT PRIMARY KEY,value INTEGER NOT NULL)");
    auto resetTable=sql::execute(db,"SELECT 1 FROM sqlite_master WHERE type='table' AND name='episode_status_resets'");
    const bool hadResets=resetTable.next();resetTable.finish();
    sql::execute(db, "CREATE TABLE IF NOT EXISTS episode_status_resets(show_key TEXT NOT NULL,season INTEGER NOT NULL,episode INTEGER NOT NULL,revision INTEGER NOT NULL,PRIMARY KEY(show_key,season,episode))");
    if(!hadResets)sql::execute(db,"INSERT INTO episode_status_resets SELECT show_key,season,episode,MAX(revision) FROM segments "
        "WHERE kind IN ('recap','intro','credits','preview') GROUP BY show_key,season,episode HAVING COUNT(*)=4 AND SUM(deleted)=4 AND MAX(revision)>0");
    if (value == 0) sql::execute(db, "PRAGMA user_version=1");
}

qint64 currentRevision(QSqlDatabase &db)
{
    auto table=sql::execute(db,"SELECT 1 FROM sqlite_master WHERE type='table' AND name='segments'");
    if(!table.next())return 0;
    auto row=sql::execute(db,"SELECT COALESCE(MAX(revision),0) FROM segments");row.next();return row.value(0).toLongLong();
}

void copyAliases(QSqlDatabase &db, const providers::ShowSummary &before, const providers::ShowSummary &after)
{
    for (const auto &target : showKeys(after)) for (const auto &source : showKeys(before)) {
        sql::execute(db, "INSERT INTO segments SELECT ?,season,episode,kind,start,end,deleted,source,provider,provider_episode,updated,revision "
            "FROM segments WHERE show_key=? AND 1 ON CONFLICT(show_key,season,episode,kind) DO UPDATE SET "
            "start=excluded.start,end=excluded.end,deleted=excluded.deleted,source=excluded.source,provider=excluded.provider,"
            "provider_episode=excluded.provider_episode,updated=excluded.updated,revision=excluded.revision "
            "WHERE excluded.revision>segments.revision", {target, source});
        sql::execute(db,"INSERT INTO episode_status_resets SELECT ?,season,episode,revision FROM episode_status_resets WHERE show_key=? AND 1 "
            "ON CONFLICT(show_key,season,episode) DO UPDATE SET revision=MAX(episode_status_resets.revision,excluded.revision)",{target,source});
    }
}

} // namespace introvert::segments
