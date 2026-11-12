#include "settings_store.h"
#include "endpoints.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <utility>

namespace introvert {
namespace {

const QStringList providers = {"plex", "emby", "jellyfin", "local"};
const QStringList accounts = {"media:plex", "media:emby", "media:jellyfin",
                             "endpoints:introdb_app", "endpoints:theintrodb_org", "endpoints:skipdb_tv",
                             "metadata:tmdb", "metadata:tvdb", "metadata:omdb", "metadata:fanart", "webhook:key",
                             "metadata:movies:tmdb", "metadata:movies:tvdb", "metadata:movies:omdb", "metadata:movies:fanart"};
const QStringList endpointFields = {"introdb_enabled", "introdb_api_url", "introdb_lookup_url",
    "theintrodb_enabled", "theintrodb_base_url", "api_timeout_ms", "api_delay_seconds",
    "save_submissions_local_db", "skipdb_enabled", "skipdb_base_url"};
const QString skipDbBaseUrl = QStringLiteral("https://api.skipdb.tv");

void require(bool condition, const char *message)
{
    if (!condition) throw SettingsError(message);
}

using sql::Connection;
using sql::Transaction;
using sql::execute;
using sql::restrictPermissions;
using sql::text;

bool containsControls(const QString &value)
{
    for (const auto c : value) if (c.unicode() < 32 || c.unicode() == 127) return true;
    return false;
}

bool validServerUrl(const QString &value)
{
    if (value.isEmpty()) return true;
    const QUrl url(value, QUrl::StrictMode);
    if (!url.isValid() || (url.scheme() != "http" && url.scheme() != "https")
        || url.host().isEmpty() || url.authority().contains('@') || !url.query().isEmpty()
        || !url.fragment().isEmpty() || url.port() == 0 || value.contains('\\')
        || containsControls(value)) return false;
    for (const auto c : value) if (c.isSpace()) return false;
    if (url.host().contains(':')) return true; // QUrl already validates IPv6.
    auto host = QString::fromLatin1(QUrl::toAce(url.host()));
    if (host.endsWith('.')) host.chop(1);
    static const QRegularExpression label(QStringLiteral("^[A-Za-z0-9_](?:[A-Za-z0-9_-]{0,61}[A-Za-z0-9_])?$"));
    for (const auto &part : host.split('.')) if (!label.match(part).hasMatch()) return false;
    return true;
}

void validateSetting(const QString &field, const QVariant &value)
{
    QStringList options;
    if (field == "timestamp_format") options = {"seconds", "MM:SS", "both"};
    else if (field == "tv_show_listing_type") options = {"full", "basic"};
    else if (field == "media_provider") options = {"", "plex", "emby", "jellyfin", "local"};
    else throw SettingsError("Unsupported application setting.");
    require(value.metaType().id() == QMetaType::QString && options.contains(value.toString()),
            "Invalid application setting.");
}

QSet<QString> columns(QSqlDatabase &db, const QString &table)
{
    // Table identifiers are private constants, never form input.
    auto query = execute(db, "PRAGMA table_info(" + table + ")");
    QSet<QString> result;
    while (query.next()) result.insert(query.value(1).toString());
    return result;
}

AppSettings loadApp(QSqlDatabase &db)
{
    auto query = execute(db, QStringLiteral("SELECT timestamp_format,tv_show_listing_type,media_provider FROM app_settings WHERE id=1"));
    require(query.next(), "INTROvert app_settings row is missing.");
    for (int i : {0, 1, 2}) require(query.value(i).metaType().id() == QMetaType::QString,
                                  "Incompatible application settings values.");
    AppSettings result{query.value(0).toString(), query.value(1).toString(), query.value(2).toString()};
    validateSetting("timestamp_format", result.timestampFormat);
    validateSetting("tv_show_listing_type", result.listingType);
    validateSetting("media_provider", result.mediaProvider);
    return result;
}

ProviderSettings loadProviderFrom(QSqlDatabase &db, const QString &provider)
{
    require(providers.contains(provider), "Unsupported provider identifier.");
    auto query = execute(db, QStringLiteral("SELECT p.remote_url,p.local_root,p.fetch_images,p.timeout_seconds,p.import_segments,"
                                           "COALESCE(l.import_mode,'tv'),COALESCE(l.import_movie_segments,0) FROM provider_settings p "
                                           "LEFT JOIN provider_library_settings l ON l.provider_id=p.provider_id WHERE p.provider_id=?"), {provider});
    require(query.next(), "Provider configuration is missing or incompatible.");
    require(query.value(0).metaType().id() == QMetaType::QString
                && query.value(1).metaType().id() == QMetaType::QString,
            "Incompatible provider settings values.");
    const auto images = query.value(2);
    const auto timeout = query.value(3);
    const auto imports = query.value(4), movieImports = query.value(6);
    for (const auto &preference : {imports, movieImports})
        require((preference.metaType().id() == QMetaType::Int || preference.metaType().id() == QMetaType::LongLong)
                    && (preference.toLongLong() == 0 || preference.toLongLong() == 1), "Invalid timestamp import preference in app.db.");
    require((images.metaType().id() == QMetaType::Int || images.metaType().id() == QMetaType::LongLong)
                && (images.toLongLong() == 0 || images.toLongLong() == 1), "Invalid image preference in app.db.");
    require(timeout.metaType().id() == QMetaType::Double || timeout.metaType().id() == QMetaType::Int
                || timeout.metaType().id() == QMetaType::LongLong, "Invalid timeout in app.db.");
    ProviderSettings result{provider, query.value(0).toString(), query.value(1).toString(),
                            images.toBool(), timeout.toDouble(), imports.toBool(), query.value(5).toString(), movieImports.toBool()};
    query.finish();
    if (provider == "local") {
        auto folders = execute(db, "SELECT path,type FROM local_media_folders ORDER BY position");
        while (folders.next()) result.localFolders.append({folders.value(0).toString(), folders.value(1).toString()});
    }
    result.validate();
    return result;
}

void writeCredential(QSqlDatabase &db, const QString &account, const std::optional<QString> &value)
{
    require(accounts.contains(account), "Unsupported credential account.");
    auto query = execute(db, QStringLiteral("UPDATE credentials SET value=? WHERE account=?"),
                         {value ? QVariant(text(*value)) : QVariant(QMetaType::fromType<QString>()), account});
    require(query.numRowsAffected() == 1, "Credential account is missing from app.db.");
}

QVariantMap loadEndpointsFrom(QSqlDatabase &db)
{
    auto query = execute(db, "SELECT " + endpointFields.join(',') + " FROM endpoint_settings WHERE id=1");
    require(query.next(), "Endpoint configuration is missing.");
    QVariantMap result;
    for (int i = 0; i < endpointFields.size(); ++i) {
        const auto &name = endpointFields[i];
        auto value = query.value(i);
        if (name.endsWith("_enabled") || name == "save_submissions_local_db") {
            require((value.metaType().id() == QMetaType::Int || value.metaType().id() == QMetaType::LongLong)
                        && (value.toLongLong() == 0 || value.toLongLong() == 1), "Incompatible endpoint switches.");
            value = value.toBool();
        }
        result.insert(name, value);
    }
    SettingsStore::validateEndpoints(result);
    return result;
}

} // namespace

QStringList ProviderSettings::localPaths(const QString &type) const
{
    QStringList result;
    for (const auto &folder : localFolders) if (folder.type == type) result.append(folder.path);
    if (type == "tv" && localFolders.isEmpty() && !localRoot.isEmpty()) result.append(localRoot);
    return result;
}

void ProviderSettings::validate() const
{
    require(providers.contains(provider), "Unsupported provider identifier.");
    require(importMode == "tv" || (provider != "local" && (importMode == "movies" || importMode == "both")),
            "Unsupported media import preference.");
    require(std::isfinite(timeoutSeconds) && timeoutSeconds > 0,
            "Request timeout must be a finite positive number of seconds.");
    require(!containsControls(remoteUrl) && !containsControls(localRoot), "Address or path contains invalid characters.");
    if (provider == "local") {
        require(remoteUrl.isEmpty(), "Local media uses a directory, not a server URL.");
        static const QRegularExpression windowsRoot(QStringLiteral(R"(^(?:[A-Za-z]:[\\/]|\\\\[^\\/]+[\\/][^\\/]+))"));
        require(localRoot.isEmpty() || localRoot.startsWith('/') || windowsRoot.match(localRoot).hasMatch(),
                "Use an absolute local directory path, or leave it empty.");
        QStringList paths;
        for (const auto &folder : localFolders) {
            require(folder.type == "tv" || folder.type == "movies", "Choose TV Shows or Movies for the Local folder.");
            require(!containsControls(folder.path) && (folder.path.startsWith('/') || windowsRoot.match(folder.path).hasMatch()),
                "Use an absolute Local directory path.");
            auto path = folder.path;
            bool windows = windowsRoot.match(path).hasMatch() || path.startsWith("//");
#ifdef Q_OS_WIN
            windows = true;
#endif
            if (windows) path.replace('\\', '/');
            const bool unc = path.startsWith("//");
            path = QDir::cleanPath(path);
            if (unc && !path.startsWith("//")) path.prepend('/');
            if (windows) path = path.toCaseFolded();
            if (!path.endsWith('/')) path += '/';
            for (const auto &previous : paths)
                require(!path.startsWith(previous) && !previous.startsWith(path), "Choose Local folders that do not overlap.");
            paths.append(path);
        }
    } else {
        require(localRoot.isEmpty(), "Remote providers do not use a local root.");
        require(validServerUrl(remoteUrl), "Use an HTTP/HTTPS server URL without user-info, query parameters, or fragments. "
                "Enter tokens and API keys only in the credential field.");
    }
}

SettingsStore::SettingsStore(QString databasePath) : m_path(std::move(databasePath))
{
    require(!m_path.isEmpty(), "INTROvert database path is empty.");
}

void SettingsStore::initialize() const
{
    require(QDir().mkpath(QFileInfo(m_path).absolutePath()), "Cannot create INTROvert application data directory.");
    Connection connection(m_path);
    auto &db = connection.db;
    execute(db, QStringLiteral("PRAGMA journal_mode=WAL"));
    restrictPermissions(m_path);
    auto versionQuery = execute(db, QStringLiteral("PRAGMA user_version"));
    require(versionQuery.next(), "Cannot read INTROvert database version.");
    const int version = versionQuery.value(0).toInt();
    versionQuery.finish();
    require(version >= 0 && version <= 2, "Unsupported INTROvert database version.");
    Transaction transaction(db);
    if (version == 0) {
        execute(db, QStringLiteral("CREATE TABLE IF NOT EXISTS app_settings ("
            "id INTEGER PRIMARY KEY CHECK (id = 1),theme_mode TEXT NOT NULL DEFAULT 'auto',"
            "timestamp_format TEXT NOT NULL DEFAULT 'both',tv_show_listing_type TEXT NOT NULL DEFAULT 'full',"
            "media_provider TEXT NOT NULL DEFAULT '')"));
        execute(db, QStringLiteral("INSERT OR IGNORE INTO app_settings(id) VALUES(1)"));
    }
    if (columns(db,"app_settings").contains("intro_detection_min_seconds"))
        execute(db,QStringLiteral("ALTER TABLE app_settings DROP COLUMN intro_detection_min_seconds"));
    loadApp(db);
    require(columns(db, "app_settings") == QSet<QString>{"id", "theme_mode", "timestamp_format", "tv_show_listing_type",
                "media_provider"}, "Incompatible application settings schema.");
    if (version < 2) {
        execute(db, QStringLiteral("CREATE TABLE provider_settings (provider_id TEXT PRIMARY KEY NOT NULL "
            "CHECK(provider_id IN ('plex','emby','jellyfin','local')),remote_url TEXT NOT NULL DEFAULT '',"
            "local_root TEXT NOT NULL DEFAULT '',fetch_images INTEGER NOT NULL DEFAULT 0 CHECK(fetch_images IN (0,1)),"
            "timeout_seconds REAL NOT NULL DEFAULT 10 CHECK(timeout_seconds > 0),"
            "CHECK((provider_id='local' AND remote_url='') OR (provider_id!='local' AND local_root='')))"));
        for (const auto &provider : providers)
            execute(db, QStringLiteral("INSERT INTO provider_settings(provider_id) VALUES(?)"), {provider});
    }
    if (!columns(db, "provider_settings").contains("import_segments"))
        execute(db, "ALTER TABLE provider_settings ADD COLUMN import_segments INTEGER NOT NULL DEFAULT 0 CHECK(import_segments IN (0,1))");
    require(columns(db, "provider_settings") == QSet<QString>{"provider_id", "remote_url", "local_root", "fetch_images", "timeout_seconds", "import_segments"},
            "Incompatible provider settings schema.");
    execute(db, "CREATE TABLE IF NOT EXISTS provider_library_settings("
        "provider_id TEXT PRIMARY KEY NOT NULL REFERENCES provider_settings(provider_id),"
        "import_mode TEXT NOT NULL CHECK(import_mode IN ('tv','movies','both')),"
        "import_movie_segments INTEGER NOT NULL DEFAULT 0 CHECK(import_movie_segments IN (0,1)))");
    if (!columns(db, "provider_library_settings").contains("import_movie_segments"))
        execute(db, "ALTER TABLE provider_library_settings ADD COLUMN import_movie_segments INTEGER NOT NULL DEFAULT 0 CHECK(import_movie_segments IN (0,1))");
    execute(db, "CREATE TABLE IF NOT EXISTS local_media_folders("
        "path TEXT PRIMARY KEY NOT NULL,type TEXT NOT NULL CHECK(type IN ('tv','movies')),position INTEGER NOT NULL)");
    execute(db, "INSERT OR IGNORE INTO local_media_folders SELECT local_root,'tv',0 FROM provider_settings "
        "WHERE provider_id='local' AND local_root<>'' AND NOT EXISTS(SELECT 1 FROM local_media_folders)");
    for (const auto &provider : providers) loadProviderFrom(db, provider);

    execute(db, QStringLiteral("CREATE TABLE IF NOT EXISTS credentials(account TEXT PRIMARY KEY,value TEXT)"));
    for (const auto &account : accounts) {
        if (account.startsWith("metadata:movies:")) {
            const auto legacy = QString(account).replace("metadata:movies:", "metadata:");
            execute(db, "INSERT OR IGNORE INTO credentials(account,value) SELECT ?,value FROM credentials WHERE account=?", {account, legacy});
        }
        execute(db, "INSERT OR IGNORE INTO credentials(account,value) VALUES(?,NULL)", {account});
    }
    execute(db, QStringLiteral("CREATE TABLE IF NOT EXISTS endpoint_settings (id INTEGER PRIMARY KEY CHECK(id=1),"
        "introdb_enabled INTEGER NOT NULL CHECK(introdb_enabled IN (0,1)),introdb_api_url TEXT NOT NULL,"
        "introdb_lookup_url TEXT NOT NULL,theintrodb_enabled INTEGER NOT NULL CHECK(theintrodb_enabled IN (0,1)),"
        "theintrodb_base_url TEXT NOT NULL,api_timeout_ms INTEGER NOT NULL,api_delay_seconds INTEGER NOT NULL,"
        "save_submissions_local_db INTEGER NOT NULL CHECK(save_submissions_local_db IN (0,1)))"));
    const auto existingEndpointFields = columns(db, "endpoint_settings");
    if (!existingEndpointFields.contains("skipdb_enabled"))
        execute(db, "ALTER TABLE endpoint_settings ADD COLUMN skipdb_enabled INTEGER NOT NULL DEFAULT 0 CHECK(skipdb_enabled IN (0,1))");
    if (!existingEndpointFields.contains("skipdb_base_url"))
        execute(db, "ALTER TABLE endpoint_settings ADD COLUMN skipdb_base_url TEXT NOT NULL DEFAULT '" + skipDbBaseUrl + "'");
    QVariantList defaults;
    QStringList placeholders;
    const auto endpointValues = endpointDefaults();
    for (const auto &name : endpointFields) {
        defaults.append(endpointValues.value(name));
        placeholders.append("?");
    }
    execute(db, "INSERT OR IGNORE INTO endpoint_settings(id," + endpointFields.join(',') + ") VALUES(1," + placeholders.join(',') + ')', defaults);
    execute(db,"UPDATE endpoint_settings SET theintrodb_base_url='https://api.theintrodb.org/v4' WHERE theintrodb_base_url IN "
        "('https://api.theintrodb.org/v3','https://api.theintrodb.org/v3/','https://api.theintrodb.org/v2','https://api.theintrodb.org/v2/')");
    execute(db,"CREATE TABLE IF NOT EXISTS local_metadata_settings(root TEXT PRIMARY KEY,provider TEXT NOT NULL,artwork TEXT NOT NULL,episode_order TEXT NOT NULL)");
    execute(db, "CREATE TABLE IF NOT EXISTS local_library_metadata_settings("
        "type TEXT PRIMARY KEY CHECK(type IN ('tv','movies')),provider TEXT NOT NULL,artwork TEXT NOT NULL,episode_order TEXT NOT NULL)");
    execute(db, "INSERT OR IGNORE INTO local_library_metadata_settings "
        "SELECT f.type,CASE WHEN f.type='movies' AND s.provider='tvmaze' THEN 'tmdb' ELSE s.provider END,"
        "CASE WHEN f.type='movies' AND s.artwork='tvmaze' THEN 'same' ELSE s.artwork END,"
        "CASE WHEN f.type='movies' THEN 'official' ELSE s.episode_order END "
        "FROM local_media_folders f JOIN local_metadata_settings s ON s.root=f.path OR s.root=rtrim(replace(f.path,char(92),'/'),'/') "
        "WHERE f.position=(SELECT MIN(position) FROM local_media_folders WHERE type=f.type)");

    loadEndpointsFrom(db);
    execute(db, QStringLiteral("CREATE TABLE IF NOT EXISTS webhook_settings(id INTEGER PRIMARY KEY CHECK(id=1),enabled INTEGER NOT NULL,url TEXT NOT NULL)"));
    execute(db, QStringLiteral("INSERT OR IGNORE INTO webhook_settings VALUES(1,0,'')"));
    execute(db, QStringLiteral("CREATE TABLE IF NOT EXISTS update_settings(id INTEGER PRIMARY KEY CHECK(id=1),"
        "automatic INTEGER NOT NULL DEFAULT 0 CHECK(automatic IN (0,1)),checked_at INTEGER NOT NULL DEFAULT 0)"));
    execute(db, QStringLiteral("INSERT OR IGNORE INTO update_settings(id) VALUES(1)"));
    if (!columns(db, "update_settings").contains("latest_application_version"))
        execute(db, QStringLiteral("ALTER TABLE update_settings ADD COLUMN latest_application_version TEXT NOT NULL DEFAULT ''"));
    if (!columns(db, "update_settings").contains("latest_detector_version"))
        execute(db, QStringLiteral("ALTER TABLE update_settings ADD COLUMN latest_detector_version TEXT NOT NULL DEFAULT ''"));
    execute(db, QStringLiteral("CREATE TABLE IF NOT EXISTS detection_settings(id INTEGER PRIMARY KEY CHECK(id=1),"
        "resource_profile TEXT NOT NULL DEFAULT 'medium' CHECK(resource_profile IN ('low','medium','full')),"
        "visual_refinement_enabled INTEGER NOT NULL DEFAULT 1 CHECK(visual_refinement_enabled IN (0,1)))"));
    execute(db, QStringLiteral("INSERT OR IGNORE INTO detection_settings(id) VALUES(1)"));
    if (!columns(db, "detection_settings").contains("visual_refinement_enabled"))
        execute(db, QStringLiteral("ALTER TABLE detection_settings ADD COLUMN visual_refinement_enabled INTEGER NOT NULL DEFAULT 1 CHECK(visual_refinement_enabled IN (0,1))"));
    execute(db, QStringLiteral("CREATE TABLE IF NOT EXISTS in_app_notification_settings(id INTEGER PRIMARY KEY CHECK(id=1),"
        "enabled INTEGER NOT NULL DEFAULT 1 CHECK(enabled IN (0,1)),"
        "detection_complete INTEGER NOT NULL DEFAULT 1 CHECK(detection_complete IN (0,1)),"
        "errors INTEGER NOT NULL DEFAULT 1 CHECK(errors IN (0,1)),"
        "application_updates INTEGER NOT NULL DEFAULT 1 CHECK(application_updates IN (0,1)),"
        "detector_updates INTEGER NOT NULL DEFAULT 1 CHECK(detector_updates IN (0,1)))"));
    execute(db, QStringLiteral("INSERT OR IGNORE INTO in_app_notification_settings(id) VALUES(1)"));
    execute(db, QStringLiteral("PRAGMA user_version=2"));
    transaction.commit();
    restrictPermissions(m_path);
}

AppSettings SettingsStore::load() const
{
    Connection connection(m_path);
    return loadApp(connection.db);
}

QVariantMap SettingsStore::loadLocalMetadata(const QString &root, const QString &type) const
{
    Connection connection(m_path);
    if (!type.isEmpty()) {
        require(type == "tv" || type == "movies", "Unsupported Local library type.");
        auto settings = execute(connection.db, "SELECT provider,artwork,episode_order FROM local_library_metadata_settings WHERE type=?", {type});
        if (settings.next()) return {{"provider",settings.value(0)},{"artwork",settings.value(1)},{"episode_order",settings.value(2)}};
    }
    auto query=execute(connection.db,"SELECT provider,artwork,episode_order FROM local_metadata_settings WHERE root=?",{root});
    if(query.next())return {{"provider",query.value(0)},{"artwork",query.value(1)},{"episode_order",query.value(2)}};
    query.finish();
    auto tables=execute(connection.db,"SELECT 1 FROM sqlite_master WHERE name='local_show_metadata'");
    bool existing=false;
    if(tables.next()) {
        tables.finish();
        auto saved=execute(connection.db,"SELECT 1 FROM local_show_metadata m JOIN shows s ON s.id=m.show_id WHERE s.root=? LIMIT 1",{root});
        existing=saved.next();
    }
    return {{"provider",existing?"tvmaze":"tmdb"},{"artwork","same"},{"episode_order","official"}};
}

QVariantMap SettingsStore::loadLocalLibraryMetadata(const QString &type) const
{
    require(type == "tv" || type == "movies", "Unsupported Local library type.");
    QString root;
    {
        Connection connection(m_path);
        auto first = execute(connection.db, "SELECT s.root FROM local_media_folders f JOIN local_metadata_settings s "
            "ON s.root=f.path OR s.root=rtrim(replace(f.path,char(92),'/'),'/') "
            "WHERE f.type=? ORDER BY f.position LIMIT 1", {type});
        if (first.next()) root = first.value(0).toString();
    }
    auto settings = loadLocalMetadata(root, type);
    if (type == "movies") {
        if (settings.value("provider") == "tvmaze") settings["provider"] = "tmdb";
        if (settings.value("artwork") == "tvmaze") settings["artwork"] = "same";
        settings["episode_order"] = "official";
    }
    return settings;
}

void SettingsStore::saveLocalLibraryMetadata(const QString &type, const QVariantMap &values) const
{
    require(type == "tv" || type == "movies", "Unsupported Local library type.");
    require(values.size() == 3, "Invalid Local library settings.");
    require((type == "tv" ? QStringList{"tmdb","tvdb","omdb","tvmaze"} : QStringList{"tmdb","tvdb","omdb"})
        .contains(values.value("provider").toString()), "Unsupported metadata provider.");
    require((type == "tv" ? QStringList{"same","off","tmdb","tvdb","omdb","tvmaze","fanart"}
                         : QStringList{"same","off","tmdb","tvdb","omdb","fanart"})
        .contains(values.value("artwork").toString()), "Unsupported artwork provider.");
    require(QStringList{"default","official","dvd","alternate","absolute","regional"}
        .contains(values.value("episode_order").toString()), "Unsupported episode order.");
    require(type == "tv" || values.value("episode_order") == "official", "Movies do not use episode ordering.");
    Connection connection(m_path);
    execute(connection.db, "INSERT INTO local_library_metadata_settings VALUES(?,?,?,?) ON CONFLICT(type) DO UPDATE SET "
        "provider=excluded.provider,artwork=excluded.artwork,episode_order=excluded.episode_order",
        {type,values.value("provider"),values.value("artwork"),values.value("episode_order")});
}

void SettingsStore::saveLocalMetadata(const QString &root, const QVariantMap &values) const
{
    saveLocalMetadata({{root, values}});
}

void SettingsStore::saveLocalMetadata(const QMap<QString, QVariantMap> &folders) const
{
    Connection connection(m_path);
    Transaction transaction(connection.db);
    for (auto it = folders.cbegin(); it != folders.cend(); ++it) {
        const auto &values = it.value();
        require(!it.key().isEmpty() && values.size() == 3, "Choose a Local media folder first.");
        require(QStringList{"tmdb","tvdb","omdb","tvmaze"}.contains(values.value("provider").toString()), "Unsupported metadata provider.");
        require(QStringList{"same","off","tmdb","tvdb","omdb","tvmaze","fanart"}.contains(values.value("artwork").toString()), "Unsupported artwork provider.");
        require(QStringList{"default","official","dvd","alternate","absolute","regional"}.contains(values.value("episode_order").toString()), "Unsupported episode order.");
        execute(connection.db, "INSERT INTO local_metadata_settings VALUES(?,?,?,?) ON CONFLICT(root) DO UPDATE SET "
            "provider=excluded.provider,artwork=excluded.artwork,episode_order=excluded.episode_order",
            {it.key(), values.value("provider"), values.value("artwork"), values.value("episode_order")});
    }
    transaction.commit();
}

QString SettingsStore::detectionResources() const
{
    Connection connection(m_path);
    // Older, isolated detector databases can predate application preferences.
    auto table=execute(connection.db,"SELECT name FROM sqlite_master WHERE type='table' AND name='detection_settings'");
    if (!table.next()) return "medium";
    auto row=execute(connection.db,"SELECT resource_profile FROM detection_settings WHERE id=1");
    require(row.next(),"Detection resource preferences are missing."); const auto profile=row.value(0).toString();
    require(QStringList{"low","medium","full"}.contains(profile),"Invalid detection resource preference."); return profile;
}
void SettingsStore::setDetectionResources(const QString &profile) const
{
    require(QStringList{"low","medium","full"}.contains(profile),"Invalid detection resource preference.");
    Connection connection(m_path);
    auto row=execute(connection.db,"UPDATE detection_settings SET resource_profile=? WHERE id=1",{profile});
    require(row.numRowsAffected()==1,"Detection resource preferences are missing.");
}

bool SettingsStore::visualRefinementEnabled() const
{
    Connection connection(m_path);
    if (!columns(connection.db,"detection_settings").contains("visual_refinement_enabled")) return true;
    auto row=execute(connection.db,"SELECT visual_refinement_enabled FROM detection_settings WHERE id=1");
    require(row.next(),"Visual refinement preference is missing.");
    const auto value=row.value(0);
    require((value.metaType().id()==QMetaType::Int || value.metaType().id()==QMetaType::LongLong)
        && (value.toLongLong()==0 || value.toLongLong()==1),"Invalid visual refinement preference.");
    return value.toBool();
}
void SettingsStore::setVisualRefinementEnabled(bool enabled) const
{
    Connection connection(m_path);
    auto row=execute(connection.db,"UPDATE detection_settings SET visual_refinement_enabled=? WHERE id=1",{enabled?1:0});
    require(row.numRowsAffected()==1,"Visual refinement preference is missing.");
}

QVariantMap SettingsStore::inAppNotificationDefaults()
{
    return {{"enabled", true}, {"detection_complete", true}, {"errors", true}, {"application_updates", true}, {"detector_updates", true}};
}
QVariantMap SettingsStore::loadInAppNotifications() const
{
    Connection connection(m_path);
    auto row = execute(connection.db, "SELECT enabled,detection_complete,errors,application_updates,detector_updates FROM in_app_notification_settings WHERE id=1");
    require(row.next(), "In-app notification preferences are missing.");
    return {{"enabled", row.value(0).toBool()}, {"detection_complete", row.value(1).toBool()}, {"errors", row.value(2).toBool()},
        {"application_updates", row.value(3).toBool()}, {"detector_updates", row.value(4).toBool()}};
}
void SettingsStore::setInAppNotificationPreference(const QString &name, bool enabled) const
{
    require(inAppNotificationDefaults().contains(name), "Unknown in-app notification preference.");
    Connection connection(m_path);
    auto row = execute(connection.db, "UPDATE in_app_notification_settings SET " + name + "=? WHERE id=1", {enabled ? 1 : 0});
    require(row.numRowsAffected() == 1, "In-app notification preferences are missing.");
}

QVariantMap SettingsStore::loadUpdates() const
{
    Connection connection(m_path);
    auto row = execute(connection.db, "SELECT automatic,checked_at,latest_application_version,latest_detector_version FROM update_settings WHERE id=1");
    require(row.next(), "Application update preferences are missing.");
    return {{"automatic", row.value(0).toBool()}, {"checked_at", row.value(1).toLongLong()},
        {"latest_application_version", row.value(2).toString()}, {"latest_detector_version", row.value(3).toString()}};
}
void SettingsStore::setAutomaticUpdates(bool automatic) const
{
    Connection connection(m_path);
    execute(connection.db, "UPDATE update_settings SET automatic=? WHERE id=1", {automatic});
}
void SettingsStore::recordUpdateCheck(qint64 checkedAt) const
{
    Connection connection(m_path);
    execute(connection.db, "UPDATE update_settings SET checked_at=? WHERE id=1", {checkedAt});
}
void SettingsStore::recordLatestUpdateVersions(const QString &application, const QString &detector) const
{
    Connection connection(m_path);
    execute(connection.db, "UPDATE update_settings SET latest_application_version=COALESCE(NULLIF(?,''),latest_application_version),"
        "latest_detector_version=COALESCE(NULLIF(?,''),latest_detector_version) WHERE id=1", {application, detector});
}

AppSettings SettingsStore::update(const QString &field, const QVariant &value) const
{
    validateSetting(field, value);
    Connection connection(m_path);
    Transaction transaction(connection.db);
    const auto binding = value.metaType().id() == QMetaType::QString ? QVariant(text(value.toString())) : value;
    auto query = execute(connection.db, "UPDATE app_settings SET " + field + "=? WHERE id=1", {binding});
    require(query.numRowsAffected() == 1, "INTROvert app_settings row is missing.");
    const auto result = loadApp(connection.db);
    transaction.commit();
    return result;
}

ProviderSettings SettingsStore::loadProvider(const QString &provider) const
{
    Connection connection(m_path);
    return loadProviderFrom(connection.db, provider);
}

std::optional<QString> SettingsStore::providerCredential(const ProviderSettings &saved) const
{
    Connection connection(m_path);
    auto query = execute(connection.db, "SELECT p.remote_url,c.value FROM provider_settings p "
        "LEFT JOIN credentials c ON c.account=? WHERE p.provider_id=?", {"media:" + saved.provider, saved.provider});
    require(query.next() && query.value(0).toString() == saved.remoteUrl,
        "The media connection changed. Start this operation again with the current connection.");
    return query.value(1).isNull() ? std::nullopt : std::optional<QString>(query.value(1).toString());
}

void SettingsStore::saveProvider(const ProviderSettings &settings, CredentialAction action, const QString &key) const
{
    Connection connection(m_path);
    Transaction transaction(connection.db);
    auto updated = settings;
    if (updated.provider == "local") {
        const auto saved = loadProviderFrom(connection.db, "local");
        if (updated.localFolders.isEmpty() && !updated.localRoot.isEmpty()) {
            updated.localFolders = {{updated.localRoot, "tv"}};
        } else if (updated.localFolders == saved.localFolders && updated.localRoot != saved.localRoot) {
            // Older settings callers replace the first TV folder through localRoot.
            const auto firstTv = std::find_if(updated.localFolders.begin(), updated.localFolders.end(),
                [](const LocalFolder &folder) { return folder.type == "tv"; });
            if (firstTv != updated.localFolders.end()) {
                if (updated.localRoot.isEmpty()) updated.localFolders.erase(firstTv);
                else firstTv->path = updated.localRoot;
            } else if (!updated.localRoot.isEmpty()) updated.localFolders.prepend({updated.localRoot, "tv"});
        }
        updated.localRoot.clear();
        updated.localRoot = updated.localPaths("tv").value(0);
    }
    updated.validate();
    require(action == CredentialAction::Keep || action == CredentialAction::Replace || action == CredentialAction::Delete,
            "Unsupported credential action.");
    require(settings.provider != "local" || (action == CredentialAction::Keep && key.isEmpty()), "Local media does not use credentials.");
    require(action != CredentialAction::Keep || key.isEmpty(), "Choose Replace credential to save a new credential.");
    require(action != CredentialAction::Replace || !key.trimmed().isEmpty(), "Enter a nonempty replacement credential.");
    auto query = execute(connection.db, QStringLiteral("UPDATE provider_settings SET remote_url=?,local_root=?,fetch_images=?,"
        "timeout_seconds=?,import_segments=? WHERE provider_id=?"), {text(updated.remoteUrl), text(updated.localRoot), updated.fetchImages, updated.timeoutSeconds, updated.importSegments, updated.provider});
    require(query.numRowsAffected() == 1, "Provider configuration is missing.");
    if (updated.provider == "local") {
        execute(connection.db, "DELETE FROM local_media_folders");
        for (qsizetype index = 0; index < updated.localFolders.size(); ++index) {
            const auto &folder = updated.localFolders[index];
            execute(connection.db, "INSERT INTO local_media_folders VALUES(?,?,?)", {folder.path, folder.type, index});
        }
    }
    execute(connection.db, "INSERT INTO provider_library_settings(provider_id,import_mode,import_movie_segments) VALUES(?,?,?) "
        "ON CONFLICT(provider_id) DO UPDATE SET import_mode=excluded.import_mode,import_movie_segments=excluded.import_movie_segments",
        {settings.provider, settings.importMode, settings.importMovieSegments});
    if (action != CredentialAction::Keep)
        writeCredential(connection.db, "media:" + settings.provider,
                        action == CredentialAction::Replace ? std::optional<QString>(key) : std::nullopt);
    transaction.commit();
}

QVariantMap SettingsStore::endpointDefaults()
{
    return {{"introdb_enabled", true}, {"introdb_api_url", "https://api.introdb.app/submit"},
        {"introdb_lookup_url", "https://api.introdb.app/segments"}, {"theintrodb_enabled", false},
        {"theintrodb_base_url", "https://api.theintrodb.org/v4"}, {"api_timeout_ms", 10000},
        {"api_delay_seconds", 5}, {"save_submissions_local_db", true},
        {"skipdb_enabled", false}, {"skipdb_base_url", skipDbBaseUrl}};
}

void SettingsStore::validateEndpoints(const QVariantMap &values)
{
    require(values.keys() == endpointDefaults().keys(), "Incompatible endpoint configuration fields.");
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        const auto &name = it.key();
        const auto &value = it.value();
        if (name.endsWith("_enabled") || name == "save_submissions_local_db")
            require(value.metaType().id() == QMetaType::Bool, "Endpoint switches must be boolean.");
        else if (name == "api_timeout_ms" || name == "api_delay_seconds")
            require((value.metaType().id() == QMetaType::Int || value.metaType().id() == QMetaType::LongLong)
                        && value.toLongLong() >= -2147483647 && value.toLongLong() <= 2147483647,
                    "Timeout and delay must be whole numbers.");
        else require(value.metaType().id() == QMetaType::QString && validServerUrl(value.toString()),
                     "Use an HTTP/HTTPS endpoint URL without credentials, query parameters or fragments.");
    }
    require(values["api_timeout_ms"].toInt() > 0, "API Timeout must be greater than zero.");
    require(!values["introdb_enabled"].toBool() || (!values["introdb_api_url"].toString().isEmpty()
                && !values["introdb_lookup_url"].toString().isEmpty()), "IntroDB requires submission and lookup URLs.");
    require(!values["theintrodb_enabled"].toBool() || !values["theintrodb_base_url"].toString().isEmpty(), "TheIntroDB requires a URL.");
    require(!values["skipdb_enabled"].toBool() || !values["skipdb_base_url"].toString().isEmpty(), "SkipDB requires a URL.");
}

QVariantMap SettingsStore::loadEndpoints() const
{
    Connection connection(m_path);
    return loadEndpointsFrom(connection.db);
}

void SettingsStore::saveEndpoints(const QVariantMap &values, const QMap<QString, QString> &editedKeys) const
{
    validateEndpoints(values);
    for (auto it = editedKeys.cbegin(); it != editedKeys.cend(); ++it)
        require(endpointById(it.key())
                    && !it.value().contains('\r') && !it.value().contains('\n'), "Invalid endpoint credential.");
    Connection connection(m_path);
    Transaction transaction(connection.db);
    QStringList assignments;
    QVariantList bindings;
    for (const auto &name : endpointFields) {
        assignments.append(name + "=?");
        const auto value = values[name];
        bindings.append(value.metaType().id() == QMetaType::QString ? QVariant(text(value.toString())) : value);
    }
    auto query = execute(connection.db, "UPDATE endpoint_settings SET " + assignments.join(',') + " WHERE id=1", bindings);
    require(query.numRowsAffected() == 1, "Endpoint configuration is missing.");
    for (auto it = editedKeys.cbegin(); it != editedKeys.cend(); ++it)
        writeCredential(connection.db, "endpoints:" + it.key(), it.value().isEmpty() ? std::nullopt : std::optional<QString>(it.value()));
    transaction.commit();
}

bool SettingsStore::validWebhookUrl(const QString &value)
{
    const QUrl url(value, QUrl::StrictMode);
    if (!url.isValid() || (url.scheme() != "http" && url.scheme() != "https") || url.host().isEmpty()
        || !url.userName().isEmpty() || !url.password().isEmpty() || url.port() == 0 || containsControls(value)) return false;
    for (const auto c : value) if (c.isSpace()) return false;
    return true;
}

QVariantMap SettingsStore::loadWebhook() const
{
    Connection connection(m_path);
    auto query = execute(connection.db, QStringLiteral("SELECT enabled,url FROM webhook_settings WHERE id=1"));
    require(query.next(), "Webhook configuration is missing.");
    return {{"enabled", query.value(0).toBool()}, {"url", query.value(1).toString()}};
}

QVariantMap SettingsStore::saveWebhook(const QVariantMap &values, const std::optional<QString> &editedKey) const
{
    require(values.keys() == QStringList{"enabled", "url"} && values["enabled"].metaType().id() == QMetaType::Bool
                && values["url"].metaType().id() == QMetaType::QString, "Invalid webhook configuration.");
    const auto url = text(values["url"].toString().trimmed());
    require(!values["enabled"].toBool() || validWebhookUrl(url), "Enter a valid HTTP or HTTPS webhook URL.");
    const auto key = editedKey ? editedKey->trimmed() : QString();
    require(!key.contains('\r') && !key.contains('\n'), "Webhook key must not contain line breaks.");
    Connection connection(m_path);
    Transaction transaction(connection.db);
    auto query = execute(connection.db, QStringLiteral("UPDATE webhook_settings SET enabled=?,url=? WHERE id=1"), {values["enabled"], url});
    require(query.numRowsAffected() == 1, "Webhook configuration is missing.");
    if (editedKey) writeCredential(connection.db, "webhook:key", key.isEmpty() ? std::nullopt : std::optional<QString>(key));
    transaction.commit();
    return {{"enabled", values["enabled"]}, {"url", url}};
}

std::optional<QString> SettingsStore::credential(const QString &account) const
{
    require(accounts.contains(account), "Unsupported credential account.");
    Connection connection(m_path);
    auto query = execute(connection.db, QStringLiteral("SELECT value FROM credentials WHERE account=?"), {account});
    require(query.next(), "Credential account is missing from app.db.");
    if (query.value(0).isNull()) return std::nullopt;
    return query.value(0).toString();
}

bool SettingsStore::hasCredential(const QString &account) const
{
    const auto key = credential(account);
    return key && !key->isEmpty();
}

void SettingsStore::setCredential(const QString &account, const std::optional<QString> &value) const
{
    Connection connection(m_path);
    Transaction transaction(connection.db);
    writeCredential(connection.db, account, value);
    transaction.commit();
}

} // namespace introvert
