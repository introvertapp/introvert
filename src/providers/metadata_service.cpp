#include "metadata_service.h"
#include "json_values.h"
#include "segments/identity.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <chrono>

namespace introvert::providers {
namespace {
bool truthy(const QJsonValue &value)
{
    if (value.isNull() || value.isUndefined()) return false;
    if (value.isBool()) return value.toBool();
    if (value.isString()) return !value.toString().isEmpty();
    if (value.isArray()) return !value.toArray().isEmpty();
    if (value.isObject()) return !value.toObject().isEmpty();
    return value.toDouble() != 0;
}
QJsonObject objectOrEmpty(const QJsonValue &value)
{
    if (truthy(value) && !value.isObject()) throw std::invalid_argument("Invalid metadata object");
    return value.toObject();
}
QVariant nullable(const std::optional<QString> &value)
{
    return value ? QVariant(sql::text(*value)) : QVariant(QMetaType::fromType<QString>());
}
std::optional<QString> truthyString(const QJsonValue &value)
{
    if (value.isString() && !value.toString().isEmpty()) return value.toString();
    return std::nullopt;
}
QString imageUrl(const QJsonArray &images, const QString &kind)
{
    for (const auto &value : images) {
        if (!value.isObject()) throw std::invalid_argument("Invalid metadata image");
        const auto image = value.toObject();
        if (image["type"] != kind) continue;
        const auto resolutions = objectOrEmpty(image["resolutions"]);
        const auto selected = truthy(resolutions["original"]) ? resolutions["original"]
            : truthy(resolutions["medium"]) ? resolutions["medium"] : QJsonValue(resolutions);
        return objectOrEmpty(selected)["url"].toString();
    }
    return {};
}
bool wholeNumber(const QJsonValue &value)
{
    const auto type = value.toVariant().metaType().id();
    return type == QMetaType::LongLong || type == QMetaType::Int;
}
}

MetadataService::MetadataService(QString databasePath, library::MediaCache &cache, MetadataClient client)
    : m_path(std::move(databasePath)), m_provider(m_path), m_cache(cache), m_client(std::move(client)), m_settings(m_path)
{
    m_provider.initializeCatalog();
    sql::Connection connection(m_path);
    sql::execute(connection.db, "CREATE TABLE IF NOT EXISTS local_metadata_jobs(root TEXT PRIMARY KEY,status TEXT NOT NULL,"
        "current INTEGER NOT NULL,total INTEGER NOT NULL,title TEXT,message TEXT,error TEXT,updated REAL NOT NULL)");
}

QVariantMap MetadataService::status(const QString &root) const
{
    sql::Connection connection(m_path);
    auto query = sql::execute(connection.db, "SELECT root,status,current,total,title,message,error,updated FROM local_metadata_jobs WHERE root=?", {root});
    if (!query.next()) return {};
    const QStringList fields{"root", "status", "current", "total", "title", "message", "error", "updated"};
    QVariantMap result;
    for (int i = 0; i < fields.size(); ++i) result[fields[i]] = query.value(i);
    return result;
}
QVariantMap MetadataService::record(const QString &root, const QString &state, qint64 current, qint64 total,
                                   const QString &title, const QString &message, const QString &error) const
{
    sql::Connection connection(m_path);
    sql::execute(connection.db, "INSERT OR REPLACE INTO local_metadata_jobs VALUES(?,?,?,?,?,?,?,?)",
        {root, state, current, total, sql::text(title), sql::text(message), sql::text(error),
         std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count()});
    return status(root);
}
QVariantMap MetadataService::recover(const QString &root) const
{
    const auto value = status(root);
    if (QStringList{"running", "queued", "canceling"}.contains(value["status"].toString()))
        return record(root, "failed", value["current"].toLongLong(), value["total"].toLongLong(), {}, "Previous metadata fetch was interrupted");
    return value;
}

QVariantMap MetadataService::run(const ProviderConfig &config, const Cancellation &cancelled, const Progress &progress)
{
    const auto root = LocalProvider::rootPath(config);
    qint64 current = 0, total = 0;
    auto report = [&](const QString &state, const QString &title = QString(), const QString &message = QString(), const QString &error = QString()) {
        const auto result = record(root, state, current, total, title, message, error);
        if (progress) progress(result);
        return result;
    };
    try {
        report("running", {}, "Scanning local media");
        m_provider.invalidate(); m_provider.listSeries(config, true, cancelled);
        QList<QPair<QString, QString>> shows;
        {
            sql::Connection connection(m_path);
            auto query = sql::execute(connection.db, "SELECT id,title FROM shows WHERE root=? ORDER BY title", {root});
            while (query.next()) shows.append({query.value(0).toString(), query.value(1).toString()});
        }
        total = shows.size();
        const auto key = m_settings.credential("metadata:tmdb");
        const auto timeout = m_settings.loadEndpoints()["api_timeout_ms"].toDouble() / 1000;
        {
            sql::Connection connection(m_path);
            segments::initializeTables(connection.db);
        }
        for (const auto &row : shows) {
            checkCancelled(cancelled);
            report("running", row.second, "Currently updating: " + row.second);
            const auto detail = m_provider.getShow(config, row.first, false, cancelled);
            if (!detail) throw DatabaseError("Local catalog changed during metadata fetch");
            const auto before = detail->show;
            std::optional<QString> previousTmdb, poster, logo;
            {
                sql::Connection connection(m_path);
                auto query = sql::execute(connection.db, "SELECT tmdb,poster,logo FROM local_show_metadata WHERE show_id=?", {row.first});
                if (query.next()) {
                    if (!query.value(0).isNull()) previousTmdb = query.value(0).toString();
                    if (!query.value(1).isNull()) poster = query.value(1).toString();
                    if (!query.value(2).isNull()) logo = query.value(2).toString();
                }
            }
            try {
                const auto value = m_client.tv("/singlesearch/shows", {{"q", row.second}, {"embed", "episodes"}}, cancelled);
                if (!value.isObject()) throw ProviderError(QStringLiteral("TVmaze returned an invalid show response"));
                const auto body = value.toObject(), external = objectOrEmpty(body["externals"]);
                if ((truthy(external["imdb"]) && !external["imdb"].isString()) || (truthy(body["name"]) && !body["name"].isString()))
                    throw std::invalid_argument("Invalid metadata identifier or title");
                const auto imdb = truthyString(external["imdb"]);
                const auto tvdb = MetadataClient::positiveId(external["thetvdb"]), tvmaze = MetadataClient::positiveId(body["id"]);
                static const QRegularExpression validImdb(QStringLiteral("^tt\\d{7,8}$"), QRegularExpression::UseUnicodePropertiesOption);
                const auto match = imdb ? validImdb.match(*imdb) : QRegularExpressionMatch();
                const auto lookupImdb = imdb && match.hasMatch() && match.capturedLength() == imdb->size() ? imdb : std::nullopt;
                const auto found = m_client.tmdbId(lookupImdb, tvdb, key, timeout, cancelled);
                auto tmdb = found.id ? found.id : previousTmdb;
                if (tmdb && tmdb->isEmpty()) tmdb.reset();
                std::optional<QString> error = found.error;
                QString state;
                if (imdb && tmdb && !tmdb->isEmpty()) { state = "complete"; error.reset(); }
                else if ((imdb || tvdb) && (!tmdb || tmdb->isEmpty())) {
                    state = "missing_tmdb"; if (!error || error->isEmpty()) error = "TMDB match not found";
                } else if (tmdb && !tmdb->isEmpty()) { state = "missing_imdb"; error = "TVmaze match did not include an IMDb ID"; }
                else { state = "missing_external_ids"; error = "TVmaze match did not include IMDb or TVDB IDs for TMDB lookup"; }
                if (config.fetchImages && tvmaze && *tvmaze) {
                    try {
                        const auto scope = m_cache.scope(config, "images");
                        auto have = [&](const std::optional<QString> &url) { return url && !url->isEmpty() && m_cache.image(scope, *url, true).has_value(); };
                        if (!have(poster) || !have(logo)) {
                            const auto images = m_client.tv("/shows/" + QString::number(*tvmaze) + "/images", {}, cancelled).toArray();
                            if (!have(poster)) {
                                const auto image = objectOrEmpty(body["image"]);
                                auto url = image["medium"].toString();
                                if (url.isEmpty()) url = image["original"].toString();
                                if (url.isEmpty()) url = imageUrl(images, "poster");
                                poster = url.isEmpty() ? std::nullopt : std::optional<QString>(url);
                            }
                            if (!have(logo)) {
                                const auto url = imageUrl(images, "typography");
                                logo = url.isEmpty() ? std::nullopt : std::optional<QString>(url);
                            }
                            for (const auto &url : {poster, logo})
                                if (url && !url->isEmpty() && !have(url)) m_cache.putImage(scope, *url, MetadataClient::image(*url, cancelled));
                        }
                    } catch (const ProviderError &) { /* Artwork failures retain identifiers/titles. */ }
                }
                checkCancelled(cancelled);
                auto after = before;
                after.title = body["name"].toString(); if (after.title.isEmpty()) after.title = row.second;
                after.externalIds = {imdb, tmdb, tvdb ? std::optional<QString>(QString::number(*tvdb)) : std::nullopt,
                                      {}, tvmaze ? std::optional<QString>(QString::number(*tvmaze)) : std::nullopt};
                sql::Connection connection(m_path); auto &db = connection.db; sql::Transaction transaction(db);
                sql::execute(db, "INSERT OR REPLACE INTO local_show_metadata VALUES(?,?,?,?,?,?,?,?,?,?)",
                    {row.first, after.title, nullable(imdb), nullable(tmdb), nullable(after.externalIds.tvdb),
                     nullable(after.externalIds.tvmaze), nullable(poster), nullable(logo), state, nullable(error)});
                const auto embedded = objectOrEmpty(body["_embedded"]);
                const auto episodes = embedded.contains("episodes") ? embedded["episodes"] : QJsonValue(QJsonArray{});
                if (!episodes.isArray()) throw std::invalid_argument("Invalid metadata episode list");
                for (const auto &value : episodes.toArray()) {
                    if (!value.isObject()) throw std::invalid_argument("Invalid metadata episode");
                    const auto episode = value.toObject();
                    if (wholeNumber(episode["season"]) && wholeNumber(episode["number"]) && !episode["name"].toString().isEmpty())
                        sql::execute(db, "INSERT OR REPLACE INTO local_episode_metadata VALUES(?,?,?,?)",
                            {row.first, episode["season"].toInteger(), episode["number"].toInteger(), episode["name"].toString()});
                }
                segments::copyAliases(db, before, after);
                transaction.commit();
            } catch (const ProviderError &error) {
                sql::Connection connection(m_path);
                sql::execute(connection.db, "INSERT INTO local_show_metadata(show_id,status,error) VALUES(?,?,?) ON CONFLICT(show_id) "
                    "DO UPDATE SET status=excluded.status,error=excluded.error",
                    {row.first, dynamic_cast<const MetadataNotFound *>(&error) ? "not_found" : "failed", QString::fromUtf8(error.what())});
            }
            ++current;
            report("running", {}, "Updated " + QString::number(current) + " / " + QString::number(total) + " shows");
        }
        return report("complete", {}, total ? "Metadata fetch complete" : "No TV episodes found");
    } catch (const Cancelled &) { return report("canceled", {}, "Metadata fetch stopped"); }
    catch (...) { return report("failed", {}, "Metadata fetch failed", "Cannot update Local metadata. Check app.db and network access."); }
}

} // namespace introvert::providers
