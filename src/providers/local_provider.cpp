#include "local_provider.h"
#include "local_names.h"
#include "metadata_client.h"
#include "access_retry.h"
#include "embedded_segments.h"
#include "core/sqlite.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <cmath>

namespace introvert::providers {
namespace {
std::optional<QString> optionalString(const QVariant &value)
{
    return value.isNull() ? std::nullopt : std::optional<QString>(value.toString());
}
QString hash(const QString &value) { return QString::fromLatin1(QCryptographicHash::hash(value.toUtf8(), QCryptographicHash::Sha1).toHex()); }
QString childPath(const QString &parent, const QString &name)
{
    auto path = QDir::fromNativeSeparators(parent);
    if (!path.endsWith('/')) path += '/';
    return QDir::toNativeSeparators(path + name);
}
std::optional<QList<ImportedSegment>> cachedSegments(const QString &payload)
{
    const auto document=QJsonDocument::fromJson(payload.toUtf8());
    if(!document.isArray())return {};
    QList<ImportedSegment> result;
    for(const auto &value:document.array()) {
        const auto row=value.toArray();
        if(row.size()!=3 || !row[0].isString() || !row[1].isDouble() || !row[2].isDouble())return {};
        const auto kind=row[0].toString();const auto start=row[1].toDouble(),end=row[2].toDouble();
        if(!QStringList{"recap","intro","credits","preview"}.contains(kind) || !std::isfinite(start) || !std::isfinite(end) || start<0 || end<=start)return {};
        result.append({segmentType(kind),start,end});
    }
    return result;
}
QString segmentPayload(const QList<ImportedSegment> &segments)
{
    QJsonArray rows;
    for(const auto &segment:segments)rows.append(QJsonArray{segmentName(segment.type),segment.startSeconds,segment.endSeconds});
    return QString::fromUtf8(QJsonDocument(rows).toJson(QJsonDocument::Compact));
}
}

LocalProvider::LocalProvider(QString databasePath, Inspector inspector)
    : m_path(std::move(databasePath)),m_inspector(inspector ? std::move(inspector) : inspectEmbeddedSegments) {}

void LocalProvider::initializeCatalog()
{
    if (!QDir().mkpath(QFileInfo(m_path).absolutePath())) throw DatabaseError("Cannot open Local catalog in app.db.");
    sql::Connection connection(m_path);
    initialize(connection.db);
}

QString LocalProvider::rootPath(const ProviderConfig &config, const Cancellation &cancelled)
{
    checkCancelled(cancelled);
    if (config.provider != ProviderId::Local || !config.localRoot || config.localRoot->isEmpty())
        throw ProviderError(QStringLiteral("Choose a media folder."));
    auto path = QDir::fromNativeSeparators(*config.localRoot);
    if (path == "~" || path.startsWith("~/")) path.replace(0, 1, QDir::homePath());
    if (!QDir::isAbsolutePath(path)) path = QDir::currentPath() + '/' + path;
    // Match pathlib's lexical absolute form: collapse "." and redundant slashes
    // but preserve ".." and symlink spellings used by existing catalog IDs.
    QStringList parts;
    for (const auto &part : path.split('/', Qt::SkipEmptyParts)) if (part != ".") parts.append(part);
    const auto prefix = path.startsWith("//") && !path.startsWith("///") ? "//" : path.startsWith('/') ? "/" : "";
    auto absolute = prefix + parts.join('/');
#ifdef Q_OS_WIN
    // pathlib retains the separator on drive roots and UNC share roots.
    if ((absolute.size() == 2 && absolute[1] == ':') || (absolute.startsWith("//") && parts.size() == 2)) absolute += '/';
#endif
    return QDir::toNativeSeparators(absolute);
}

void LocalProvider::initialize(QSqlDatabase &db)
{
    auto versionQuery = sql::execute(db, "PRAGMA user_version"); versionQuery.next();
    const auto version = versionQuery.value(0).toInt(); versionQuery.finish();
    if (version < 0 || version > 2) throw ProviderError(QStringLiteral("Unsupported Local catalog version."));
    sql::execute(db, "CREATE TABLE IF NOT EXISTS shows(id TEXT PRIMARY KEY,root TEXT NOT NULL,path TEXT NOT NULL,title TEXT NOT NULL,year INTEGER)");
    sql::execute(db, "CREATE TABLE IF NOT EXISTS episodes(id TEXT PRIMARY KEY,show_id TEXT NOT NULL,season INTEGER NOT NULL,number INTEGER NOT NULL,title TEXT,path TEXT NOT NULL)");
    sql::execute(db, "CREATE TABLE IF NOT EXISTS local_show_metadata(show_id TEXT PRIMARY KEY,title TEXT,imdb TEXT,tmdb TEXT,tvdb TEXT,tvmaze TEXT,poster TEXT,logo TEXT,status TEXT,error TEXT)");
    sql::execute(db, "CREATE TABLE IF NOT EXISTS local_episode_metadata(show_id TEXT,season INTEGER,number INTEGER,title TEXT,PRIMARY KEY(show_id,season,number))");
    sql::execute(db,"CREATE TABLE IF NOT EXISTS local_metadata_matches(show_id TEXT,provider TEXT,provider_id TEXT,PRIMARY KEY(show_id,provider))");
    sql::execute(db,"CREATE TABLE IF NOT EXISTS local_episode_addresses(episode_id TEXT PRIMARY KEY,source TEXT,episode_order TEXT,tvdb_episode_id TEXT)");
    sql::execute(db, "CREATE INDEX IF NOT EXISTS shows_root ON shows(root)");
    sql::execute(db, "CREATE INDEX IF NOT EXISTS episodes_show ON episodes(show_id)");
    sql::execute(db,"CREATE TABLE IF NOT EXISTS local_episode_media(episode_id TEXT PRIMARY KEY,root TEXT NOT NULL,size INTEGER NOT NULL,modified_ms INTEGER NOT NULL,"
        "inspected INTEGER NOT NULL DEFAULT 0,duration_ms INTEGER,segments TEXT NOT NULL DEFAULT '[]')");
    auto roots=sql::execute(db,"SELECT 1 FROM sqlite_master WHERE type='table' AND name='local_catalog_roots'");
    const bool hadRoots=roots.next();roots.finish();
    sql::execute(db,"CREATE TABLE IF NOT EXISTS local_catalog_roots(root TEXT PRIMARY KEY)");
    // Existing inventories are already authoritative, including before the
    // persistent empty-root marker was introduced. No NAS access is needed.
    if(!hadRoots)sql::execute(db,"INSERT OR IGNORE INTO local_catalog_roots SELECT DISTINCT root FROM shows");
    if (version == 0) sql::execute(db, "PRAGMA user_version=1");
}

void LocalProvider::scan(QSqlDatabase &db, const QString &root, const Cancellation &cancelled)
{
    if (!readableMediaDirectory(root,cancelled)) throw ProviderError(QStringLiteral("Local media path is not a readable directory. Check the folder or mounted drive."));
    struct Known {QString id;qint64 size,modified;bool signature;};
    QMap<QString,Known> known;
    auto stored=sql::execute(db,"SELECT e.id,e.path,m.size,m.modified_ms FROM episodes e JOIN shows s ON s.id=e.show_id "
        "LEFT JOIN local_episode_media m ON m.episode_id=e.id WHERE s.root=?",{root});
    while(stored.next())known[stored.value(1).toString()]={stored.value(0).toString(),stored.value(2).toLongLong(),stored.value(3).toLongLong(),!stored.value(2).isNull()};
    stored.finish();
    static const QSet<QString> videos{"mkv", "mp4", "m4v", "avi", "mov", "wmv", "flv", "webm", "ts", "m2ts", "mpg", "mpeg"};
    static const QSet<QString> skipped{"@eadir", ".appledouble", ".ds_store", "sample", "samples", "subs", "subtitles", "extras", "featurettes"};
    QStringList stack{root}, files;bool complete=true;
    QMap<QString,QPair<qint64,qint64>> signatures;
    while (!stack.isEmpty() && files.size() < 20000) {
        checkCancelled(cancelled);
        const auto path = stack.takeLast(); const QDir directory(path);
        if (!directory.isReadable()) {
            if (path == root) throw ProviderError(QStringLiteral("Local media path is not readable. Check folder permissions."));
            complete=false;continue;
        }
        QDirIterator entries(path, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        while (entries.hasNext()) {
            checkCancelled(cancelled);
            entries.next();
            const auto entry = entries.fileInfo();
            const auto name = entry.fileName();
            if (name.startsWith('.') || entry.isSymLink()) continue;
            const auto file = childPath(path, name);
            if (entry.isDir()) { if (!skipped.contains(name.toLower())) stack.append(file); }
            else if (entry.isFile() && videos.contains(entry.suffix().toLower())) {
                files.append(file);signatures[file]={entry.size(),entry.lastModified().toMSecsSinceEpoch()};
                if (files.size() >= 20000) {complete=false;break;}
            }
        }
    }
    std::stable_sort(files.begin(), files.end(), [](const auto &a, const auto &b) { return a.toCaseFolded() < b.toCaseFolded(); });
    if(!readableMediaDirectory(root,cancelled))throw ProviderError("Local media path became unavailable during the rescan. Saved catalog retained.");
    sql::Transaction transaction(db);
    sql::execute(db,"CREATE TEMP TABLE scanned_local_episodes(id TEXT PRIMARY KEY)");
    static const QRegularExpression normalize(QStringLiteral("[^a-z0-9]+"));
    for (const auto &file : files) {
        checkCancelled(cancelled);
        const auto signature=signatures.value(file);
        const auto prior=known.constFind(file);
        if(prior!=known.cend() && prior->signature && prior->size==signature.first && prior->modified==signature.second) {
            sql::execute(db,"INSERT OR IGNORE INTO scanned_local_episodes VALUES(?)",{prior->id});continue;
        }
        const auto parsed = parseEpisode(file, root);
        if (!parsed) continue;
        const auto normalized = cleanTitle(parsed->showTitle).toLower().replace(normalize, " ").trimmed();
        if (normalized.isEmpty()) continue;
        const auto id = hash(root + ':' + normalized);
        const auto parts = QDir(root).relativeFilePath(file).split('/');
        const auto showPath = parts.size() > 1 ? childPath(root, parts.first()) : root;
        sql::execute(db, "INSERT INTO shows VALUES(?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET path=excluded.path,year=COALESCE(shows.year,excluded.year) "
            "WHERE shows.path IS NOT excluded.path OR (shows.year IS NULL AND excluded.year IS NOT NULL)",
            {id, root, showPath, parsed->showTitle, parsed->year ? QVariant(*parsed->year) : QVariant(QMetaType::fromType<qint64>())});
        const auto episodeId = hash(file + ':' + QString::number(parsed->seasonNumber) + ':' + QString::number(parsed->episodeNumber));
        sql::execute(db,"INSERT OR IGNORE INTO scanned_local_episodes VALUES(?)",{episodeId});
        sql::execute(db, "INSERT INTO episodes VALUES(?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET title=excluded.title",
            {episodeId, id, parsed->seasonNumber, parsed->episodeNumber,
             parsed->episodeTitle ? QVariant(*parsed->episodeTitle) : QVariant(QMetaType::fromType<QString>()), file});
        sql::execute(db,"INSERT INTO local_episode_media(episode_id,root,size,modified_ms) VALUES(?,?,?,?) ON CONFLICT(episode_id) DO UPDATE SET "
            "inspected=CASE WHEN size=excluded.size AND modified_ms=excluded.modified_ms THEN inspected ELSE 0 END,"
            "duration_ms=CASE WHEN size=excluded.size AND modified_ms=excluded.modified_ms THEN duration_ms ELSE NULL END,"
            "segments=CASE WHEN size=excluded.size AND modified_ms=excluded.modified_ms THEN segments ELSE '[]' END,"
            "size=excluded.size,modified_ms=excluded.modified_ms",{episodeId,root,signature.first,signature.second});
    }
    checkCancelled(cancelled);
    if(complete) {
        sql::execute(db,"DELETE FROM local_episode_media WHERE root=? AND episode_id NOT IN(SELECT id FROM scanned_local_episodes)",{root});
        sql::execute(db,"DELETE FROM episodes WHERE show_id IN(SELECT id FROM shows WHERE root=?) AND id NOT IN(SELECT id FROM scanned_local_episodes)",{root});
    }
    sql::execute(db,"INSERT OR IGNORE INTO local_catalog_roots VALUES(?)",{root});
    transaction.commit(); m_rescan = false;
}

bool LocalProvider::hasCatalog(const ProviderConfig &config,const Cancellation &cancelled)
{
    const auto root=rootPath(config,cancelled);initializeCatalog();sql::Connection connection(m_path);
    auto query=sql::execute(connection.db,"SELECT 1 FROM local_catalog_roots WHERE root=?",{root});return query.next();
}

void LocalProvider::refreshCatalog(const ProviderConfig &config,const Cancellation &cancelled)
{
    const auto root=rootPath(config,cancelled);initializeCatalog();sql::Connection connection(m_path);scan(connection.db,root,cancelled);
}

ShowSummary LocalProvider::show(QSqlDatabase &db, const QString &id, const QString &title, const QVariant &year,
                                const QString &path, bool images)
{
    auto counts = sql::execute(db, "SELECT COUNT(DISTINCT season),COUNT(*) FROM episodes WHERE show_id=?", {id}); counts.next();
    ShowSummary result{{ProviderId::Local, id, {}}, title};
    result.seasonCount = counts.value(0).toLongLong(); result.episodeCount = counts.value(1).toLongLong();
    if (!year.isNull()) result.year = year.toLongLong();
    auto metadata = sql::execute(db, "SELECT title,imdb,tmdb,tvdb,tvmaze,poster,logo,status,error FROM local_show_metadata WHERE show_id=?", {id});
    LocalShowMetadata local{path, QStringLiteral("pending"), {}};
    if (metadata.next()) {
        if (!metadata.value(0).toString().isEmpty()) result.title = metadata.value(0).toString();
        result.externalIds = {optionalString(metadata.value(1)), optionalString(metadata.value(2)),
                              optionalString(metadata.value(3)), {}, optionalString(metadata.value(4))};
        if (images) { result.posterUrl = optionalString(metadata.value(5)); result.logoUrl = optionalString(metadata.value(6)); }
        if (!metadata.value(7).toString().isEmpty()) local.metadataStatus = metadata.value(7).toString();
        local.metadataError = optionalString(metadata.value(8));
    }
    result.localMetadata = local;
    return result;
}

QList<ShowSummary> LocalProvider::listSeries(const ProviderConfig &config, bool includeImages, const Cancellation &cancelled)
{
    const auto root = rootPath(config, cancelled);
    if (!QDir().mkpath(QFileInfo(m_path).absolutePath())) throw DatabaseError("Cannot open Local catalog in app.db.");
    sql::Connection connection(m_path); auto &db = connection.db; initialize(db);
    auto found = sql::execute(db, "SELECT 1 FROM local_catalog_roots WHERE root=?", {root});
    const auto existing = found.next(); found.finish();
    if (m_rescan || !existing) {
        try { scan(db, root, cancelled); } catch (const ProviderError &) { if (!existing) throw; }
    }
    auto query = sql::execute(db, "SELECT id,title,year,path FROM shows WHERE root=? AND EXISTS(SELECT 1 FROM episodes WHERE show_id=shows.id)", {root});
    QList<ShowSummary> result;
    while (query.next()) result.append(show(db, query.value(0).toString(), query.value(1).toString(), query.value(2),
                                           query.value(3).toString(), includeImages && config.fetchImages));
    std::stable_sort(result.begin(), result.end(), [](const auto &a, const auto &b) { return a.title.toCaseFolded() < b.title.toCaseFolded(); });
    return result;
}

std::optional<ShowDetail> LocalProvider::getShow(const ProviderConfig &config, const QString &showId, bool includeImages,
                                                const Cancellation &cancelled)
{
    return readShow(config,showId,includeImages,cancelled,false);
}

std::optional<ShowDetail> LocalProvider::inspectShow(const ProviderConfig &config,const QString &showId,bool includeImages,
                                                  const Cancellation &cancelled)
{
    return readShow(config,showId,includeImages,cancelled,true);
}

std::optional<ShowDetail> LocalProvider::readShow(const ProviderConfig &config,const QString &showId,bool includeImages,
                                               const Cancellation &cancelled,bool inspectFiles)
{
    const auto root = rootPath(config, cancelled);
    if (!QDir().mkpath(QFileInfo(m_path).absolutePath())) throw DatabaseError("Cannot open Local catalog in app.db.");
    sql::Connection connection(m_path); auto &db = connection.db; initialize(db);
    auto found = sql::execute(db, "SELECT id,title,year,path FROM shows WHERE id=? AND root=?", {showId, root});
    if (!found.next()) return std::nullopt;
    ShowDetail result{show(db, showId, found.value(1).toString(), found.value(2), found.value(3).toString(), includeImages && config.fetchImages), {}};
    found.finish();
    auto query = sql::execute(db, "SELECT e.id,e.season,e.number,CASE WHEN m.show_id IS NOT NULL THEN m.title ELSE e.title END,e.path,p.inspected,p.duration_ms,p.segments "
        ",a.source,a.episode_order,a.tvdb_episode_id FROM episodes e LEFT JOIN local_episode_metadata m ON m.show_id=e.show_id AND m.season=e.season AND m.number=e.number "
        "LEFT JOIN local_episode_media p ON p.episode_id=e.id LEFT JOIN local_episode_addresses a ON a.episode_id=e.id WHERE e.show_id=? ORDER BY e.season,e.number,e.path", {showId});
    QSet<QString> needsInspection;
    while (query.next()) {
        checkCancelled(cancelled);
        const auto season = query.value(1).toLongLong(), number = query.value(2).toLongLong();
        auto title = query.value(3).toString();
        if (title.isEmpty()) title = "Episode " + QString::number(number);
        Episode episode{{ProviderId::Local, query.value(0).toString(), {}}};
        episode.title = title; episode.seasonNumber = season; episode.episodeNumber = number; episode.mediaPath = query.value(4).toString();
        if(!query.value(8).toString().isEmpty())episode.numberingSource=query.value(8).toString();
        if(!query.value(9).toString().isEmpty())episode.episodeOrder=query.value(9).toString();
        if(!query.value(10).toString().isEmpty())episode.tvdbEpisodeId=query.value(10).toString();
        const auto cached=query.value(5).toBool() ? cachedSegments(query.value(7).toString()) : std::nullopt;
        if(cached) {
            episode.importedSegments=*cached;
            if(!query.value(6).isNull())episode.durationMs=query.value(6).toLongLong();
        } else if(inspectFiles) needsInspection.insert(episode.identity.itemId);
        if (result.seasons.isEmpty() || result.seasons.last().number != season) result.seasons.append(Season{season, {}});
        result.seasons.last().episodes.append(episode);
    }
    // Release the read snapshot before slow media I/O. Another worker may commit
    // while ffprobe runs; upgrading that old snapshot to a write would fail with
    // SQLITE_BUSY_SNAPSHOT, even with the configured busy timeout.
    query.finish();
    for(auto &season:result.seasons) for(auto &episode:season.episodes) {
        checkCancelled(cancelled);
        if(needsInspection.contains(episode.identity.itemId)) {
            const QFileInfo before(*episode.mediaPath);
            if(before.isFile()) {
                const auto size=before.size(),modified=before.lastModified().toMSecsSinceEpoch();
                const auto inspected=m_inspector(episode,cancelled);checkCancelled(cancelled);
                const QFileInfo after(*episode.mediaPath);
                if(inspected && after.isFile() && after.size()==size && after.lastModified().toMSecsSinceEpoch()==modified)
                    sql::execute(db,"INSERT INTO local_episode_media VALUES(?,?,?,?,1,?,?) ON CONFLICT(episode_id) DO UPDATE SET "
                        "size=excluded.size,modified_ms=excluded.modified_ms,inspected=1,duration_ms=excluded.duration_ms,segments=excluded.segments",
                        {episode.identity.itemId,root,size,modified,episode.durationMs?QVariant(*episode.durationMs):QVariant(),segmentPayload(episode.importedSegments)});
            }
        }
    }
    return result;
}

void LocalProvider::clear(const ProviderConfig &config, const Cancellation &cancelled)
{
    checkCancelled(cancelled);
    if (config.provider != ProviderId::Local || !config.localRoot || config.localRoot->isEmpty())
        throw ProviderError(QStringLiteral("Select a Local media path before clearing its cache."));
    const auto root = rootPath(config);
    if (!QDir().mkpath(QFileInfo(m_path).absolutePath())) throw DatabaseError("Cannot open Local catalog in app.db.");
    sql::Connection connection(m_path); auto &db = connection.db; initialize(db); sql::Transaction transaction(db);
    sql::execute(db,"DELETE FROM local_episode_addresses WHERE episode_id IN(SELECT e.id FROM episodes e JOIN shows s ON s.id=e.show_id WHERE s.root=?)",{root});
    sql::execute(db,"DELETE FROM local_metadata_matches WHERE show_id IN(SELECT id FROM shows WHERE root=?)",{root});
    sql::execute(db,"DELETE FROM local_episode_media WHERE root=?",{root});
    sql::execute(db,"DELETE FROM local_catalog_roots WHERE root=?",{root});
    for (const auto &table : {QStringLiteral("local_show_metadata"), QStringLiteral("local_episode_metadata"), QStringLiteral("episodes")})
        sql::execute(db, "DELETE FROM " + table + " WHERE show_id IN (SELECT id FROM shows WHERE root=?)", {root});
    sql::execute(db, "DELETE FROM shows WHERE root=?", {root});
    transaction.commit(); m_rescan = true;
}
QByteArray LocalProvider::artwork(const ProviderConfig &, const QString &path, const Cancellation &cancelled)
{
    return MetadataClient::image(path, cancelled);
}
QJsonValue LocalProvider::request(const ProviderConfig &, const QString &, const QVariantMap &, const Cancellation &)
{
    throw ProviderError(QStringLiteral("Unsupported Local resource"));
}

} // namespace introvert::providers
