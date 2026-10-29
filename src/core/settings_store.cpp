#include "settings_store.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUrl>
#include <cmath>
#include <utility>

namespace introvert {
namespace {

const QStringList providers = {"plex", "emby", "jellyfin", "local"};
const QStringList accounts = {"media:plex", "media:emby", "media:jellyfin",
                             "endpoints:introdb_app", "endpoints:theintrodb_org",
                             "metadata:tmdb", "webhook:key"};
const QStringList endpointFields = {"introdb_enabled", "introdb_api_url", "introdb_lookup_url",
    "theintrodb_enabled", "theintrodb_base_url", "api_timeout_ms", "api_delay_seconds",
    "save_submissions_local_db"};

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
    if (field == "intro_detection_min_seconds") {
        require(value.metaType().id() == QMetaType::Int && value.toInt() >= 5 && value.toInt() <= 120,
                "Intro minimum length must be a whole number between 5 and 120 seconds.");
        return;
    }
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
    auto query = execute(db, QStringLiteral("SELECT timestamp_format,tv_show_listing_type,"
                         "intro_detection_min_seconds,media_provider FROM app_settings WHERE id=1"));
    require(query.next(), "Introvert app_settings row is missing.");
    // SQLite's dynamic types must not silently coerce malformed persisted values.
    for (int i : {0, 1, 3}) require(query.value(i).metaType().id() == QMetaType::QString,
                                  "Incompatible application settings values.");
    const auto minimum = query.value(2);
    require((minimum.metaType().id() == QMetaType::Int || minimum.metaType().id() == QMetaType::LongLong)
                && minimum.toLongLong() >= 5 && minimum.toLongLong() <= 120,
            "Invalid Intro minimum length in app.db.");
    AppSettings result{query.value(0).toString(), query.value(1).toString(), minimum.toInt(), query.value(3).toString()};
    validateSetting("timestamp_format", result.timestampFormat);
    validateSetting("tv_show_listing_type", result.listingType);
    validateSetting("media_provider", result.mediaProvider);
    return result;
}

ProviderSettings loadProviderFrom(QSqlDatabase &db, const QString &provider)
{
    require(providers.contains(provider), "Unsupported provider identifier.");
    auto query = execute(db, QStringLiteral("SELECT remote_url,local_root,fetch_images,timeout_seconds "
                                           "FROM provider_settings WHERE provider_id=?"), {provider});
    require(query.next(), "Provider configuration is missing or incompatible.");
    require(query.value(0).metaType().id() == QMetaType::QString
                && query.value(1).metaType().id() == QMetaType::QString,
            "Incompatible provider settings values.");
    const auto images = query.value(2);
    const auto timeout = query.value(3);
    require((images.metaType().id() == QMetaType::Int || images.metaType().id() == QMetaType::LongLong)
                && (images.toLongLong() == 0 || images.toLongLong() == 1), "Invalid image preference in app.db.");
    require(timeout.metaType().id() == QMetaType::Double || timeout.metaType().id() == QMetaType::Int
                || timeout.metaType().id() == QMetaType::LongLong, "Invalid timeout in app.db.");
    ProviderSettings result{provider, query.value(0).toString(), query.value(1).toString(),
                            images.toBool(), timeout.toDouble()};
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

void ProviderSettings::validate() const
{
    require(providers.contains(provider), "Unsupported provider identifier.");
    require(std::isfinite(timeoutSeconds) && timeoutSeconds > 0,
            "Request timeout must be a finite positive number of seconds.");
    require(!containsControls(remoteUrl) && !containsControls(localRoot), "Address or path contains invalid characters.");
    if (provider == "local") {
        require(remoteUrl.isEmpty(), "Local media uses a directory, not a server URL.");
        static const QRegularExpression windowsRoot(QStringLiteral(R"(^(?:[A-Za-z]:[\\/]|\\\\[^\\/]+[\\/][^\\/]+))"));
        require(localRoot.isEmpty() || localRoot.startsWith('/') || windowsRoot.match(localRoot).hasMatch(),
                "Use an absolute local directory path, or leave it empty.");
    } else {
        require(localRoot.isEmpty(), "Remote providers do not use a local root.");
        require(validServerUrl(remoteUrl), "Use an HTTP/HTTPS server URL without user-info, query parameters, or fragments. "
                "Enter tokens and API keys only in the credential field.");
    }
}

SettingsStore::SettingsStore(QString databasePath) : m_path(std::move(databasePath))
{
    require(!m_path.isEmpty(), "Introvert database path is empty.");
}

void SettingsStore::initialize() const
{
    require(QDir().mkpath(QFileInfo(m_path).absolutePath()), "Cannot create Introvert application data directory.");
    Connection connection(m_path);
    auto &db = connection.db;
    execute(db, QStringLiteral("PRAGMA journal_mode=WAL"));
    restrictPermissions(m_path);
    auto versionQuery = execute(db, QStringLiteral("PRAGMA user_version"));
    require(versionQuery.next(), "Cannot read Introvert database version.");
    const int version = versionQuery.value(0).toInt();
    versionQuery.finish();
    require(version >= 0 && version <= 2, "Unsupported Introvert database version.");
    Transaction transaction(db);
    if (version == 0) {
        execute(db, QStringLiteral("CREATE TABLE IF NOT EXISTS app_settings ("
            "id INTEGER PRIMARY KEY CHECK (id = 1),theme_mode TEXT NOT NULL DEFAULT 'auto',"
            "timestamp_format TEXT NOT NULL DEFAULT 'both',tv_show_listing_type TEXT NOT NULL DEFAULT 'full',"
            "intro_detection_min_seconds INTEGER NOT NULL DEFAULT 15,media_provider TEXT NOT NULL DEFAULT '')"));
        execute(db, QStringLiteral("INSERT OR IGNORE INTO app_settings(id) VALUES(1)"));
    }
    loadApp(db);
    require(columns(db, "app_settings") == QSet<QString>{"id", "theme_mode", "timestamp_format", "tv_show_listing_type",
                "intro_detection_min_seconds", "media_provider"}, "Incompatible application settings schema.");
    if (version < 2) {
        execute(db, QStringLiteral("CREATE TABLE provider_settings (provider_id TEXT PRIMARY KEY NOT NULL "
            "CHECK(provider_id IN ('plex','emby','jellyfin','local')),remote_url TEXT NOT NULL DEFAULT '',"
            "local_root TEXT NOT NULL DEFAULT '',fetch_images INTEGER NOT NULL DEFAULT 0 CHECK(fetch_images IN (0,1)),"
            "timeout_seconds REAL NOT NULL DEFAULT 10 CHECK(timeout_seconds > 0),"
            "CHECK((provider_id='local' AND remote_url='') OR (provider_id!='local' AND local_root='')))"));
        for (const auto &provider : providers)
            execute(db, QStringLiteral("INSERT INTO provider_settings(provider_id) VALUES(?)"), {provider});
    }
    require(columns(db, "provider_settings") == QSet<QString>{"provider_id", "remote_url", "local_root", "fetch_images", "timeout_seconds"},
            "Incompatible provider settings schema.");
    for (const auto &provider : providers) loadProviderFrom(db, provider);

    execute(db, QStringLiteral("CREATE TABLE IF NOT EXISTS credentials(account TEXT PRIMARY KEY,value TEXT)"));
    for (const auto &account : accounts)
        execute(db, QStringLiteral("INSERT OR IGNORE INTO credentials(account,value) VALUES(?,NULL)"), {account});
    execute(db, QStringLiteral("CREATE TABLE IF NOT EXISTS endpoint_settings (id INTEGER PRIMARY KEY CHECK(id=1),"
        "introdb_enabled INTEGER NOT NULL CHECK(introdb_enabled IN (0,1)),introdb_api_url TEXT NOT NULL,"
        "introdb_lookup_url TEXT NOT NULL,theintrodb_enabled INTEGER NOT NULL CHECK(theintrodb_enabled IN (0,1)),"
        "theintrodb_base_url TEXT NOT NULL,api_timeout_ms INTEGER NOT NULL,api_delay_seconds INTEGER NOT NULL,"
        "save_submissions_local_db INTEGER NOT NULL CHECK(save_submissions_local_db IN (0,1)))"));
    QVariantList defaults;
    const auto endpointValues = endpointDefaults();
    for (const auto &name : endpointFields) defaults.append(endpointValues.value(name));
    execute(db, QStringLiteral("INSERT OR IGNORE INTO endpoint_settings VALUES(1,?,?,?,?,?,?,?,?)"), defaults);
    loadEndpointsFrom(db);
    execute(db, QStringLiteral("CREATE TABLE IF NOT EXISTS webhook_settings(id INTEGER PRIMARY KEY CHECK(id=1),enabled INTEGER NOT NULL,url TEXT NOT NULL)"));
    execute(db, QStringLiteral("INSERT OR IGNORE INTO webhook_settings VALUES(1,0,'')"));
    execute(db, QStringLiteral("CREATE TABLE IF NOT EXISTS update_settings(id INTEGER PRIMARY KEY CHECK(id=1),"
        "automatic INTEGER NOT NULL DEFAULT 0 CHECK(automatic IN (0,1)),checked_at INTEGER NOT NULL DEFAULT 0)"));
    execute(db, QStringLiteral("INSERT OR IGNORE INTO update_settings(id) VALUES(1)"));
    execute(db, QStringLiteral("CREATE TABLE IF NOT EXISTS detection_settings(id INTEGER PRIMARY KEY CHECK(id=1),"
        "resource_profile TEXT NOT NULL DEFAULT 'medium' CHECK(resource_profile IN ('low','medium','full')))"));
    execute(db, QStringLiteral("INSERT OR IGNORE INTO detection_settings(id) VALUES(1)"));
    execute(db, QStringLiteral("PRAGMA user_version=2"));
    transaction.commit();
    restrictPermissions(m_path);
}

AppSettings SettingsStore::load() const
{
    Connection connection(m_path);
    return loadApp(connection.db);
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

QVariantMap SettingsStore::loadUpdates() const
{
    Connection connection(m_path);
    auto row = execute(connection.db, "SELECT automatic,checked_at FROM update_settings WHERE id=1");
    require(row.next(), "Application update preferences are missing.");
    return {{"automatic", row.value(0).toBool()}, {"checked_at", row.value(1).toLongLong()}};
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

AppSettings SettingsStore::update(const QString &field, const QVariant &value) const
{
    validateSetting(field, value);
    Connection connection(m_path);
    Transaction transaction(connection.db);
    const auto binding = value.metaType().id() == QMetaType::QString ? QVariant(text(value.toString())) : value;
    auto query = execute(connection.db, "UPDATE app_settings SET " + field + "=? WHERE id=1", {binding});
    require(query.numRowsAffected() == 1, "Introvert app_settings row is missing.");
    const auto result = loadApp(connection.db);
    transaction.commit();
    return result;
}

ProviderSettings SettingsStore::loadProvider(const QString &provider) const
{
    Connection connection(m_path);
    return loadProviderFrom(connection.db, provider);
}

void SettingsStore::saveProvider(const ProviderSettings &settings, CredentialAction action, const QString &key) const
{
    settings.validate();
    require(action == CredentialAction::Keep || action == CredentialAction::Replace || action == CredentialAction::Delete,
            "Unsupported credential action.");
    require(settings.provider != "local" || (action == CredentialAction::Keep && key.isEmpty()), "Local media does not use credentials.");
    require(action != CredentialAction::Keep || key.isEmpty(), "Choose Replace credential to save a new credential.");
    require(action != CredentialAction::Replace || !key.trimmed().isEmpty(), "Enter a nonempty replacement credential.");
    Connection connection(m_path);
    Transaction transaction(connection.db);
    auto query = execute(connection.db, QStringLiteral("UPDATE provider_settings SET remote_url=?,local_root=?,fetch_images=?,"
        "timeout_seconds=? WHERE provider_id=?"), {text(settings.remoteUrl), text(settings.localRoot), settings.fetchImages, settings.timeoutSeconds, settings.provider});
    require(query.numRowsAffected() == 1, "Provider configuration is missing.");
    if (action != CredentialAction::Keep)
        writeCredential(connection.db, "media:" + settings.provider,
                        action == CredentialAction::Replace ? std::optional<QString>(key) : std::nullopt);
    transaction.commit();
}

QVariantMap SettingsStore::endpointDefaults()
{
    return {{"introdb_enabled", true}, {"introdb_api_url", "https://api.introdb.app/submit"},
        {"introdb_lookup_url", "https://api.introdb.app/segments"}, {"theintrodb_enabled", false},
        {"theintrodb_base_url", "https://api.theintrodb.org/v3"}, {"api_timeout_ms", 10000},
        {"api_delay_seconds", 5}, {"save_submissions_local_db", false}};
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
        require((it.key() == "introdb_app" || it.key() == "theintrodb_org")
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
