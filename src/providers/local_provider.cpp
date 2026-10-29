#include "local_provider.h"
#include "local_names.h"
#include "metadata_client.h"
#include "access_retry.h"
#include "embedded_segments.h"
#include "core/sqlite.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>

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
}

LocalProvider::LocalProvider(QString databasePath) : m_path(std::move(databasePath)) {}

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
    sql::execute(db, "CREATE INDEX IF NOT EXISTS shows_root ON shows(root)");
    sql::execute(db, "CREATE INDEX IF NOT EXISTS episodes_show ON episodes(show_id)");
    if (version == 0) sql::execute(db, "PRAGMA user_version=1");
}

void LocalProvider::scan(QSqlDatabase &db, const QString &root, const Cancellation &cancelled)
{
    if (!readableMediaDirectory(root,cancelled)) throw ProviderError(QStringLiteral("Local media path is not a readable directory. Check the folder or mounted drive."));
    static const QSet<QString> videos{"mkv", "mp4", "m4v", "avi", "mov", "wmv", "flv", "webm", "ts", "m2ts", "mpg", "mpeg"};
    static const QSet<QString> skipped{"@eadir", ".appledouble", ".ds_store", "sample", "samples", "subs", "subtitles", "extras", "featurettes"};
    QStringList stack{root}, files;
    while (!stack.isEmpty() && files.size() < 20000) {
        checkCancelled(cancelled);
        const auto path = stack.takeLast(); const QDir directory(path);
        if (!directory.isReadable()) {
            if (path == root) throw ProviderError(QStringLiteral("Local media path is not readable. Check folder permissions."));
            continue;
        }
        QDirIterator entries(path, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        while (entries.hasNext()) {
            entries.next();
            const auto entry = entries.fileInfo();
            const auto name = entry.fileName();
            if (name.startsWith('.') || entry.isSymLink()) continue;
            const auto file = childPath(path, name);
            if (entry.isDir()) { if (!skipped.contains(name.toLower())) stack.append(file); }
            else if (entry.isFile() && videos.contains(entry.suffix().toLower())) {
                files.append(file); if (files.size() >= 20000) break;
            }
        }
    }
    std::stable_sort(files.begin(), files.end(), [](const auto &a, const auto &b) { return a.toCaseFolded() < b.toCaseFolded(); });
    sql::Transaction transaction(db);
    static const QRegularExpression normalize(QStringLiteral("[^a-z0-9]+"));
    for (const auto &file : files) {
        checkCancelled(cancelled);
        const auto parsed = parseEpisode(file, root);
        if (!parsed) continue;
        const auto normalized = cleanTitle(parsed->showTitle).toLower().replace(normalize, " ").trimmed();
        if (normalized.isEmpty()) continue;
        const auto id = hash(root + ':' + normalized);
        const auto parts = QDir(root).relativeFilePath(file).split('/');
        const auto showPath = parts.size() > 1 ? childPath(root, parts.first()) : root;
        sql::execute(db, "INSERT INTO shows VALUES(?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET path=excluded.path,year=COALESCE(shows.year,excluded.year)",
            {id, root, showPath, parsed->showTitle, parsed->year ? QVariant(*parsed->year) : QVariant(QMetaType::fromType<qint64>())});
        const auto episodeId = hash(file + ':' + QString::number(parsed->seasonNumber) + ':' + QString::number(parsed->episodeNumber));
        sql::execute(db, "INSERT INTO episodes VALUES(?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET title=excluded.title",
            {episodeId, id, parsed->seasonNumber, parsed->episodeNumber,
             parsed->episodeTitle ? QVariant(*parsed->episodeTitle) : QVariant(QMetaType::fromType<QString>()), file});
    }
    transaction.commit(); m_rescan = false;
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
    auto found = sql::execute(db, "SELECT 1 FROM shows WHERE root=?", {root});
    const auto existing = found.next(); found.finish();
    if (m_rescan || !existing) {
        try { scan(db, root, cancelled); } catch (const ProviderError &) { if (!existing) throw; }
    }
    auto query = sql::execute(db, "SELECT id,title,year,path FROM shows WHERE root=?", {root});
    QList<ShowSummary> result;
    while (query.next()) result.append(show(db, query.value(0).toString(), query.value(1).toString(), query.value(2),
                                           query.value(3).toString(), includeImages && config.fetchImages));
    std::stable_sort(result.begin(), result.end(), [](const auto &a, const auto &b) { return a.title.toCaseFolded() < b.title.toCaseFolded(); });
    return result;
}

std::optional<ShowDetail> LocalProvider::getShow(const ProviderConfig &config, const QString &showId, bool includeImages,
                                                const Cancellation &cancelled)
{
    const auto root = rootPath(config, cancelled);
    if (!QDir().mkpath(QFileInfo(m_path).absolutePath())) throw DatabaseError("Cannot open Local catalog in app.db.");
    sql::Connection connection(m_path); auto &db = connection.db; initialize(db);
    auto found = sql::execute(db, "SELECT id,title,year,path FROM shows WHERE id=? AND root=?", {showId, root});
    if (!found.next()) return std::nullopt;
    ShowDetail result{show(db, showId, found.value(1).toString(), found.value(2), found.value(3).toString(), includeImages && config.fetchImages), {}};
    found.finish();
    auto query = sql::execute(db, "SELECT id,season,number,title,path FROM episodes WHERE show_id=? ORDER BY season,number,path", {showId});
    while (query.next()) {
        checkCancelled(cancelled);
        const auto season = query.value(1).toLongLong(), number = query.value(2).toLongLong();
        auto enriched = sql::execute(db, "SELECT title FROM local_episode_metadata WHERE show_id=? AND season=? AND number=?", {showId, season, number});
        auto title = enriched.next() ? enriched.value(0).toString() : query.value(3).toString();
        if (title.isEmpty()) title = "Episode " + QString::number(number);
        Episode episode{{ProviderId::Local, query.value(0).toString(), {}}};
        episode.title = title; episode.seasonNumber = season; episode.episodeNumber = number; episode.mediaPath = query.value(4).toString();
        inspectEmbeddedSegments(episode, cancelled);
        if (result.seasons.isEmpty() || result.seasons.last().number != season) result.seasons.append(Season{season, {}});
        result.seasons.last().episodes.append(episode);
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
