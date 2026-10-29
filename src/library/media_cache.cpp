#include "media_cache.h"
#include "cache_records.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonParseError>
#include <QMessageAuthenticationCode>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <chrono>

namespace introvert::library {
namespace {
double now() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }
QByteArray randomBytes()
{
    QByteArray result(32, Qt::Uninitialized);
    for (auto &byte : result) byte = char(QRandomGenerator::system()->generate() & 255);
    return result;
}
QJsonValue json(const std::optional<QString> &value) { return value ? QJsonValue(*value) : QJsonValue(QJsonValue::Null); }
void remove(const QString &path)
{
    if (QFileInfo::exists(path) && !QFile::remove(path)) throw DatabaseError("Cannot clear the image cache");
}
}

MediaCache::MediaCache(QString artworkRoot, QString databasePath, CacheLimits limits)
    : m_root(std::move(artworkRoot)), m_path(std::move(databasePath)), m_limits(limits), m_fallback(randomBytes())
{
}

std::unique_ptr<sql::Connection> MediaCache::connect()
{
    if (!QDir().mkpath(QFileInfo(m_path).absolutePath())) throw DatabaseError("Cannot access Introvert app.db.");
    auto connection = std::make_unique<sql::Connection>(m_path);
    auto &db = connection->db;
    sql::execute(db, "PRAGMA synchronous=NORMAL");
    if (m_initialized) return connection;
    sql::execute(db, "PRAGMA journal_mode=WAL");
    auto query = sql::execute(db, "PRAGMA user_version"); query.next();
    const auto version = query.value(0).toInt(); query.finish();
    if (version < 0 || version > 2) throw DatabaseError("Unsupported disposable cache version");
    sql::Transaction transaction(db);
    sql::execute(db, "CREATE TABLE IF NOT EXISTS media_cache_settings(name TEXT PRIMARY KEY,value BLOB NOT NULL)");
    sql::execute(db, "INSERT OR IGNORE INTO media_cache_settings VALUES('scope_key',?)", {randomBytes()});
    sql::execute(db, "CREATE TABLE IF NOT EXISTS media_cache_metadata(scope TEXT,kind TEXT,item TEXT,payload TEXT,created REAL,accessed REAL,PRIMARY KEY(scope,kind,item))");
    sql::execute(db, "CREATE TABLE IF NOT EXISTS media_cache_images(scope TEXT,key TEXT,filename TEXT,size INTEGER,created REAL,accessed REAL,PRIMARY KEY(scope,key))");
    auto columns = sql::execute(db, "PRAGMA table_info(media_cache_images)");
    bool superseded = false;
    while (columns.next()) if (columns.value(1).toString() == "data") superseded = true;
    columns.finish();
    if (superseded) {
        // Same disposable-development index replacement as the reference.
        // Do not import image BLOBs, or touch any tracked/canonical table.
        sql::execute(db, "DROP TABLE media_cache_images");
        sql::execute(db, "CREATE TABLE media_cache_images(scope TEXT,key TEXT,filename TEXT,size INTEGER,created REAL,accessed REAL,PRIMARY KEY(scope,key))");
    }
    sql::execute(db, "CREATE INDEX IF NOT EXISTS media_cache_images_filename ON media_cache_images(filename)");
    if (version == 0) sql::execute(db, "PRAGMA user_version=1");
    transaction.commit(); m_initialized = true;
    return connection;
}

QString MediaCache::scope(const providers::ProviderConfig &config, const QString &listing)
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    QJsonArray values{providers::providerName(config.provider), json(config.remoteUrl), json(config.credential), listing, config.fetchImages};
    if (config.provider == providers::ProviderId::Local) values.append(json(config.localRoot));
    const auto context = asciiJson(values);
    if (m_scopes.contains(context)) return m_scopes[context];
    auto key = m_fallback;
    try {
        auto connection = connect();
        auto query = sql::execute(connection->db, "SELECT value FROM media_cache_settings WHERE name='scope_key'");
        if (query.next() && !query.value(0).isNull()) key = query.value(0).toByteArray();
    } catch (const DatabaseError &) {}
    const auto result = QString::fromLatin1(QMessageAuthenticationCode::hash(context, key, QCryptographicHash::Sha256).toHex());
    m_scopes[context] = result; return result;
}

std::optional<MediaCache::CachedRecords> MediaCache::get(const QString &scope, const QString &kind, const QString &item, double ttl, bool stale)
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    try {
        auto connection = connect(); auto &db = connection->db;
        sql::Transaction transaction(db);
        auto query = sql::execute(db, "SELECT payload,created FROM media_cache_metadata WHERE scope=? AND kind=? AND item=?",
                                  {scope, kind, sql::text(item)});
        if (!query.next()) return std::nullopt;
        const auto age = now() - query.value(1).toDouble();
        if (!(age >= 0 && age <= ttl + (stale ? staleTtl : 0))) return std::nullopt;
        QJsonParseError error;
        const auto result = QJsonValue::fromJson(query.value(0).toString().toUtf8(), &error);
        if (error.error != QJsonParseError::NoError) return std::nullopt;
        CachedRecords records = kind == "series" ? CachedRecords(decodeSeries(result)) : CachedRecords(decodeDetail(result));
        query.finish();
        sql::execute(db, "UPDATE media_cache_metadata SET accessed=? WHERE scope=? AND kind=? AND item=?",
                     {now(), scope, kind, sql::text(item)});
        transaction.commit(); return records;
    } catch (const std::exception &) { return std::nullopt; }
}

std::optional<QList<providers::ShowSummary>> MediaCache::series(const QString &scope, const QString &item, double ttl, bool stale)
{
    auto value = get(scope, "series", item, ttl, stale);
    if (value) return std::get<QList<providers::ShowSummary>>(std::move(*value));
    return std::nullopt;
}
std::optional<providers::ShowDetail> MediaCache::detail(const QString &scope, const QString &item, double ttl, bool stale)
{
    auto value = get(scope, "detail", item, ttl, stale);
    if (value) return std::get<providers::ShowDetail>(std::move(*value));
    return std::nullopt;
}

bool MediaCache::put(const QString &scope, const QString &kind, const QString &item, const QJsonValue &value)
{
    const auto payload = asciiJson(value);
    if (payload.size() > m_limits.metadataBytes) return false;
    const std::lock_guard<std::mutex> lock(m_mutex);
    try {
        const auto current = now();
        auto connection = connect(); auto &db = connection->db; sql::Transaction transaction(db);
        sql::execute(db, "INSERT OR REPLACE INTO media_cache_metadata VALUES(?,?,?,?,?,?)",
                     {scope, kind, sql::text(item), QString::fromUtf8(payload), current, current});
        sql::execute(db, "DELETE FROM media_cache_metadata WHERE created<?", {current - staleTtl - std::max(seriesTtl, detailTtl)});
        auto query = sql::execute(db, "SELECT rowid,length(CAST(payload AS BLOB)) FROM media_cache_metadata ORDER BY accessed DESC");
        QList<qint64> discarded; qint64 total = 0, index = 0;
        while (query.next()) {
            total += query.value(1).toLongLong();
            if (index++ >= m_limits.metadataEntries || total > m_limits.metadataBytes) discarded.append(query.value(0).toLongLong());
        }
        query.finish();
        for (const auto row : discarded) sql::execute(db, "DELETE FROM media_cache_metadata WHERE rowid=?", {row});
        transaction.commit(); return true;
    } catch (const DatabaseError &) { return false; }
}
bool MediaCache::putSeries(const QString &scope, const QList<providers::ShowSummary> &shows, const QString &item)
{
    try { return put(scope, "series", item, encodeSeries(shows)); } catch (const std::exception &) { return false; }
}
bool MediaCache::putDetail(const QString &scope, const providers::ShowDetail &detail, const QString &item)
{
    try { return put(scope, "detail", item, encodeDetail(detail)); } catch (const std::exception &) { return false; }
}
void MediaCache::invalidateDetails(const QString &scope)
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    try { auto connection = connect(); sql::execute(connection->db, "DELETE FROM media_cache_metadata WHERE scope=? AND kind='detail'", {scope}); }
    catch (const DatabaseError &) {}
}

QString MediaCache::imageKey(const QString &scope, const QString &path)
{
    return QString::fromLatin1(QCryptographicHash::hash((scope + '\n' + path).toUtf8(), QCryptographicHash::Sha256).toHex());
}
std::optional<QString> MediaCache::imageFile(const QString &name) const
{
    static const QRegularExpression managed(QStringLiteral("^[0-9a-f]{64}\\.[0-9a-f]{16}\\.img$"));
    const auto match = managed.match(name);
    return match.hasMatch() && match.capturedLength() == name.size()
        ? std::optional<QString>(QDir(m_root).filePath(name)) : std::nullopt;
}
std::optional<QString> MediaCache::image(const QString &scope, const QString &path, bool stale)
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    try {
        auto connection = connect(); auto &db = connection->db; sql::Transaction transaction(db);
        auto query = sql::execute(db, "SELECT filename,size,created FROM media_cache_images WHERE scope=? AND key=?", {scope, imageKey(scope, path)});
        if (!query.next()) return std::nullopt;
        const auto age = now() - query.value(2).toDouble();
        if (!stale && !(age >= 0 && age <= imageTtl)) return std::nullopt;
        const auto file = imageFile(query.value(0).toString());
        if (!file || !QFileInfo(*file).isFile() || QFileInfo(*file).size() != query.value(1).toLongLong()) return std::nullopt;
        query.finish();
        sql::execute(db, "UPDATE media_cache_images SET accessed=? WHERE scope=? AND key=?", {now(), scope, imageKey(scope, path)});
        transaction.commit(); return file;
    } catch (const DatabaseError &) { return std::nullopt; }
}
std::optional<QString> MediaCache::putImage(const QString &scope, const QString &path, const QByteArray &bytes)
{
    if (bytes.isEmpty()) return std::nullopt;
    const std::lock_guard<std::mutex> lock(m_mutex);
    const auto key = imageKey(scope, path);
    const auto name = key + '.' + QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().left(16)) + ".img";
    const auto file = QDir(m_root).filePath(name);
    try {
        if (!QDir().mkpath(m_root)) return std::nullopt;
        QSaveFile output(file);
        if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit()) return std::nullopt;
        auto connection = connect(); auto &db = connection->db; sql::Transaction transaction(db);
        auto query = sql::execute(db, "SELECT filename FROM media_cache_images WHERE scope=? AND key=?", {scope, key});
        const auto old = query.next() ? query.value(0).toString() : QString(); query.finish();
        const auto current = now();
        sql::execute(db, "INSERT OR REPLACE INTO media_cache_images VALUES(?,?,?,?,?,?)", {scope, key, name, bytes.size(), current, current});
        if (old != name) if (const auto oldFile = imageFile(old)) remove(*oldFile);
        query = sql::execute(db, "SELECT rowid,filename,size FROM media_cache_images ORDER BY accessed DESC");
        QList<QPair<qint64, QString>> discarded; qint64 total = 0;
        while (query.next()) {
            total += query.value(2).toLongLong();
            if (total > m_limits.imageBytes) discarded.append({query.value(0).toLongLong(), query.value(1).toString()});
        }
        query.finish();
        for (const auto &row : discarded) {
            if (const auto oldFile = imageFile(row.second)) remove(*oldFile);
            sql::execute(db, "DELETE FROM media_cache_images WHERE rowid=?", {row.first});
        }
        transaction.commit(); return QFileInfo::exists(file) ? std::optional<QString>(file) : std::nullopt;
    } catch (const DatabaseError &) { QFile::remove(file); return std::nullopt; }
}
void MediaCache::discardImage(const QString &scope, const QString &path)
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    auto connection = connect(); auto &db = connection->db; sql::Transaction transaction(db);
    const auto key = imageKey(scope, path);
    auto query = sql::execute(db, "SELECT filename FROM media_cache_images WHERE scope=? AND key=?", {scope, key});
    const auto name = query.next() ? query.value(0).toString() : QString(); query.finish();
    if (const auto file = imageFile(name)) remove(*file);
    sql::execute(db, "DELETE FROM media_cache_images WHERE scope=? AND key=?", {scope, key});
    transaction.commit();
}
void MediaCache::clearImages(const std::optional<QString> &scope)
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    auto connection = connect(); auto &db = connection->db; sql::Transaction transaction(db);
    if (scope) {
        auto query = sql::execute(db, "SELECT filename FROM media_cache_images WHERE scope=?", {*scope});
        QStringList names; while (query.next()) names.append(query.value(0).toString()); query.finish();
        for (const auto &name : names) if (const auto file = imageFile(name)) remove(*file);
        sql::execute(db, "DELETE FROM media_cache_images WHERE scope=?", {*scope});
    } else {
        sql::execute(db, "DELETE FROM media_cache_images");
        const QDir directory(m_root);
        for (const auto &name : directory.entryList({"*.img"}, QDir::Files | QDir::System))
            if (const auto file = imageFile(name)) remove(*file);
    }
    transaction.commit();
}
QString MediaCache::stats()
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    const QDir directory(m_root);
    if (directory.exists() && !directory.isReadable()) return QStringLiteral("Unavailable");
    qint64 bytes = 0, count = 0;
    for (const auto &file : directory.entryInfoList({"*.img"}, QDir::Files)) { bytes += file.size(); ++count; }
    return QString::number(bytes / (1024.0 * 1024), 'f', 1) + " MB / " + QString::number(count) + " images";
}

} // namespace introvert::library
