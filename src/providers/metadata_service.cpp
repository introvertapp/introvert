#include "metadata_service.h"
#include "segments/identity.h"
#include "segments/edit_locks.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <algorithm>
#include <QImageReader>
#include <QBuffer>
#include <QJsonObject>
#include <QRegularExpression>
#include <type_traits>

namespace introvert::providers {
namespace {
QVariant nullable(const std::optional<QString> &value) {return value?QVariant(sql::text(*value)):QVariant(QMetaType::fromType<QString>());}
bool compatible(const ExternalIds &before,const ExternalIds &after)
{
    for(const auto &pair:{qMakePair(before.imdb,after.imdb),qMakePair(before.tmdb,after.tmdb),qMakePair(before.tvdb,after.tvdb)})
        if(pair.first && pair.second && pair.first!=pair.second)return false;
    return true;
}
}
MetadataService::MetadataService(QString databasePath,library::MediaCache &cache,MetadataClient client)
    :m_path(std::move(databasePath)),m_provider(m_path),m_cache(cache),m_client(std::move(client)),m_settings(m_path)
{
    m_provider.initializeCatalog();sql::Connection connection(m_path);
    segments::initializeTables(connection.db);
    sql::execute(connection.db,"CREATE TABLE IF NOT EXISTS local_metadata_jobs(root TEXT PRIMARY KEY,status TEXT NOT NULL,current INTEGER NOT NULL,total INTEGER NOT NULL,title TEXT,message TEXT,error TEXT,updated REAL NOT NULL)");
    sql::execute(connection.db,"CREATE TABLE IF NOT EXISTS local_metadata_results(root TEXT PRIMARY KEY,updated_count INTEGER,unmatched INTEGER,failed INTEGER,artwork_failed INTEGER)");
    sql::execute(connection.db, "CREATE TABLE IF NOT EXISTS local_metadata_refresh(item_id TEXT PRIMARY KEY,signature TEXT NOT NULL,checked INTEGER NOT NULL)");
}
QVariantMap MetadataService::status(const QString &root) const
{
    sql::Connection connection(m_path);
    auto query=sql::execute(connection.db,"SELECT root,status,current,total,title,message,error,updated FROM local_metadata_jobs WHERE root=?",{root});
    if(!query.next())return {};
    const QStringList fields{"root","status","current","total","title","message","error","updated"};QVariantMap result;
    for(int i=0;i<fields.size();++i)result[fields[i]]=query.value(i);query.finish();
    auto counts=sql::execute(connection.db,"SELECT updated_count,unmatched,failed,artwork_failed FROM local_metadata_results WHERE root=?",{root});
    if(counts.next()) {const QStringList names{"updated_count","unmatched","failed","artwork_failed"};for(int i=0;i<names.size();++i)result[names[i]]=counts.value(i);}
    return result;
}
QVariantMap MetadataService::record(const QString &root,const QString &state,qint64 current,qint64 total,
    const QString &title,const QString &message,const QString &error) const
{
    sql::Connection connection(m_path);
    sql::execute(connection.db,"INSERT OR REPLACE INTO local_metadata_jobs VALUES(?,?,?,?,?,?,?,?)",
        {root,state,current,total,sql::text(title),sql::text(message),sql::text(error),QDateTime::currentMSecsSinceEpoch()/1000.});
    return status(root);
}
QVariantMap MetadataService::recover(const QString &root) const
{
    const auto value=status(root);
    if(QStringList{"running","queued","canceling"}.contains(value["status"].toString()))
        return record(root,"interrupted",value["current"].toLongLong(),value["total"].toLongLong(),{},"Previous fetch was interrupted. Fetch again to retry.");
    return value;
}
QVariantMap MetadataService::run(const ProviderConfig &config, const Cancellation &cancel, const Progress &progress,
                                 const QString &showId, bool reuseRecent)
{
    return runFolder(config, cancel, progress, showId, false, reuseRecent);
}

QVariantMap MetadataService::runMovies(const ProviderConfig &config, const Cancellation &cancel, const Progress &progress,
                                       bool reuseRecent)
{
    return runFolder(config, cancel, progress, {}, true, reuseRecent);
}

QVariantMap MetadataService::runFolder(const ProviderConfig &config, const Cancellation &cancel, const Progress &progress,
                                       const QString &showId, bool movies, bool reuseRecent)
{
    const auto root = LocalProvider::rootPath(config);
    qint64 current = 0, total = 0, updated = 0, unmatched = 0, failed = 0, artworkFailed = 0, reused = 0;
    const auto options = m_settings.loadLocalMetadata(root, movies ? "movies" : "tv");
    const auto sources = MetadataClient::sources(options, movies);
    const auto &source = sources.first, &artworkSource = sources.second;
    const auto order = movies ? QString("official") : options.value("episode_order").toString();
    const bool imagesEnabled = config.fetchImages && artworkSource != "off";
    auto artworkConfig = config;
    if (movies) artworkConfig.localRoot = root;
    QVariantMap credentials;
    for (const auto &name : {"tmdb", "tvdb", "omdb", "fanart"})
        credentials[name] = m_settings.credential((movies ? "metadata:movies:" : "metadata:") + QString(name)).value_or(QString());

    QElapsedTimer persisted;
    auto report = [&](const QString &state, const QString &title = {}, const QString &message = {}, const QString &error = {}, const QString &itemId = {}) {
        const auto time = QDateTime::currentMSecsSinceEpoch() / 1000.;
        const QVariantMap result{{"root", root}, {"status", state}, {"current", current}, {"total", total}, {"title", title}, {"item_id", itemId},
            {"message", message}, {"error", error}, {"updated", time}, {"updated_count", updated}, {"unmatched", unmatched},
            {"failed", failed}, {"artwork_failed", artworkFailed}, {"reused", reused}, {"media", movies ? "movies" : "shows"}};
        if (state != "running" || !persisted.isValid() || persisted.elapsed() >= 250) {
            sql::Connection connection(m_path);
            sql::Transaction transaction(connection.db);
            sql::execute(connection.db, "INSERT OR REPLACE INTO local_metadata_jobs VALUES(?,?,?,?,?,?,?,?)",
                {root, state, current, total, sql::text(title), sql::text(message), sql::text(error), time});
            sql::execute(connection.db, "INSERT OR REPLACE INTO local_metadata_results VALUES(?,?,?,?,?)",
                {root, updated, unmatched, failed, artworkFailed});
            transaction.commit();
            persisted.start();
        }
        if (progress) progress(result);
        return result;
    };

    try {
        report("running", {}, "Loading saved Local inventory");
        checkCancelled(cancel);
        QList<ShowSummary> shows;
        QList<MovieSummary> movieList;
        if (movies) movieList = m_provider.movies(config, false, nullptr, cancel);
        else {
            shows = m_provider.listSeries(config, false, cancel);
            if (!showId.isEmpty()) shows.removeIf([&](const auto &show) { return show.identity.itemId != showId; });
        }
        total = movies ? movieList.size() : shows.size();
        if (total && source != "tvmaze" && credentials.value(source).toString().trimmed().isEmpty())
            return report("failed", {}, "Save the " + source.toUpper() + " API credentials in Settings → Media.");
        QString artworkFailure, artworkExample, lookupExample;
        if (imagesEnabled && artworkSource != "tvmaze" && credentials.value(artworkSource).toString().trimmed().isEmpty())
            artworkFailure = "Save the " + artworkSource + " artwork credentials in Settings → Media.";
        const auto scope = imagesEnabled ? m_cache.scope(artworkConfig, "images") : QString();

        const auto fetch = [&](const auto &item) -> std::optional<QVariantMap> {
            constexpr bool isMovie = std::is_same_v<std::decay_t<decltype(item)>, MovieSummary>;
            checkCancelled(cancel);
            const auto &itemId = item.identity.itemId;
            bool committed = false;
            try {
                std::optional<ShowDetail> detail;
                if constexpr (!isMovie) {
                    detail = m_provider.getShow(config, itemId, false, cancel);
                    if (!detail) throw ProviderError("Show was removed from the saved inventory.");
                }

                QString savedId, poster, logo, priorStatus;
                auto signatureParts = QStringList{source, order, imagesEnabled ? artworkSource : QString("off")};
                if (detail) for (const auto &season : detail->seasons) for (const auto &episode : season.episodes)
                    signatureParts.append(episode.identity.itemId);
                const auto signature = QString::fromLatin1(QCryptographicHash::hash(signatureParts.join('\n').toUtf8(), QCryptographicHash::Sha256).toHex());
                bool recent = false;
                {
                    sql::Connection connection(m_path);
                    if constexpr (!isMovie) {
                        auto match = sql::execute(connection.db, "SELECT provider_id FROM local_metadata_matches WHERE show_id=? AND provider=?", {itemId, source});
                        if (match.next()) savedId = match.value(0).toString();
                    }
                    auto art = sql::execute(connection.db, isMovie
                        ? "SELECT poster,logo,status FROM local_movie_metadata WHERE movie_id=?"
                        : "SELECT poster,logo,status FROM local_show_metadata WHERE show_id=?", {itemId});
                    if (art.next()) {
                        poster = art.value(0).toString(); logo = art.value(1).toString(); priorStatus = art.value(2).toString();
                    }
                    art.finish();
                    if (reuseRecent && showId.isEmpty() && priorStatus == "complete") {
                        const auto now = QDateTime::currentSecsSinceEpoch();
                        auto last = sql::execute(connection.db, "SELECT 1 FROM local_metadata_refresh WHERE item_id=? AND signature=? AND checked BETWEEN ? AND ?",
                            {itemId, signature, now - 24 * 60 * 60, now});
                        recent = last.next();
                    }
                }
                if (recent && (!imagesEnabled || ((poster.isEmpty() || m_cache.image(scope, poster, true))
                    && (logo.isEmpty() || m_cache.image(scope, logo, true))))) {
                    ++reused; ++current;
                    report("running", item.title, "Reusing saved metadata for " + item.title);
                    return {};
                }

                auto locks = segments::EditLocks::forDatabase(m_path);
                if (detail && locks.blockedShow(detail->show, root))
                    throw ProviderError("Metadata changes for this show are deferred while detection is queued or running. Fetch metadata again after detection finishes.");
                report("running", item.title, "Fetching " + source + ": " + item.title);
                MetadataClient::Record result;
                const bool fetchProviderArtwork = imagesEnabled && artworkSource == source;
                if constexpr (isMovie) result = m_client.identifyMovie(source, item, credentials, cancel, fetchProviderArtwork);
                else result = m_client.identify(source, *detail, savedId, order, credentials, cancel, fetchProviderArtwork);

                const auto &before = item.externalIds;
                const bool keepAliases = compatible(before, result.ids);
                if (keepAliases) {
                    if (!result.ids.imdb) result.ids.imdb = before.imdb;
                    if (!result.ids.tmdb) result.ids.tmdb = before.tmdb;
                    if (!result.ids.tvdb) result.ids.tvdb = before.tvdb;
                    if (!result.ids.tvmaze) result.ids.tvmaze = before.tvmaze;
                } else {
                    poster.clear(); logo.clear();
                }

                QString imageError = artworkFailure;
                if (imagesEnabled && artworkFailure.isEmpty()) {
                    MetadataClient::Artwork images;
                    try {
                        if constexpr (isMovie) images = m_client.movieArtwork(artworkSource, result, item, credentials, cancel);
                        else images = m_client.artwork(artworkSource, result, *detail, order, credentials, cancel);
                        imageError = images.error;
                    } catch (const ProviderError &error) {
                        imageError = QString::fromUtf8(error.what());
                        if (error.httpStatus() == 401 || error.httpStatus() == 403 || error.httpStatus() == 429) artworkFailure = imageError;
                    }
                    const auto cacheImage = [&](const QString &url, QString &saved) {
                        if (url.isEmpty()) return;
                        try {
                            if (!m_cache.image(scope, url, true)) {
                                auto bytes = MetadataClient::image(url, cancel);
                                QBuffer buffer(&bytes); buffer.open(QIODevice::ReadOnly);
                                QImageReader reader(&buffer);
                                const auto size = reader.size();
                                if (!size.isValid() || size.width() > 16384 || size.height() > 16384
                                    || qint64(size.width()) * size.height() > 40000000 || reader.read().isNull())
                                    throw ProviderError("Artwork provider returned an invalid or oversized image.");
                                if (!m_cache.putImage(scope, url, bytes)) throw ProviderError("Cannot save downloaded artwork to the local image cache.");
                            }
                            saved = url;
                        } catch (const ProviderError &error) {
                            if (imageError.isEmpty()) imageError = QString::fromUtf8(error.what());
                        }
                    };
                    cacheImage(images.poster, poster);
                    cacheImage(images.logo, logo);
                }
                if (!imageError.isEmpty()) {
                    ++artworkFailed;
                    if (artworkExample.isEmpty()) artworkExample = item.title + ": " + imageError;
                }

                std::optional<ShowSummary> after;
                std::shared_ptr<segments::EditLocks::Lease> reservation;
                if constexpr (!isMovie) {
                    after = detail->show;
                    after->title = result.title; after->externalIds = result.ids;
                    reservation = locks.editShow({detail->show, *after}, root);
                    if (!reservation)
                        throw ProviderError("Metadata changes for this show are deferred while detection is queued or running. Fetch metadata again after detection finishes.");
                }
                checkCancelled(cancel);
                sql::Connection connection(m_path);
                auto &db = connection.db;
                sql::Transaction transaction(db);
                const auto state = imageError.isEmpty() ? "complete" : "artwork_failed";
                if constexpr (isMovie) {
                    auto present = sql::execute(db, "SELECT 1 FROM local_movies WHERE id=? AND root=?", {itemId, root});
                    if (!present.next()) throw ProviderError("Movie was removed from the saved inventory.");
                    present.finish();
                    sql::execute(db, "INSERT OR REPLACE INTO local_movie_metadata VALUES(?,?,?,?,?,?,?,?,?,?)",
                        {itemId, result.title, result.year ? QVariant(*result.year) : QVariant(), nullable(result.ids.imdb),
                         nullable(result.ids.tmdb), nullable(result.ids.tvdb), sql::text(poster), sql::text(logo), state, sql::text(imageError)});
                } else {
                    if (result.year) sql::execute(db, "UPDATE shows SET year=COALESCE(year,?) WHERE id=?", {*result.year, itemId});
                    sql::execute(db, "INSERT OR REPLACE INTO local_show_metadata VALUES(?,?,?,?,?,?,?,?,?,?)",
                        {itemId, result.title, nullable(result.ids.imdb), nullable(result.ids.tmdb), nullable(result.ids.tvdb),
                         nullable(result.ids.tvmaze), sql::text(poster), sql::text(logo), state, sql::text(imageError)});
                    sql::execute(db, "INSERT INTO local_metadata_matches(show_id,provider,provider_id) VALUES(?,?,?) "
                        "ON CONFLICT(show_id,provider) DO UPDATE SET provider_id=excluded.provider_id", {itemId, source, result.id});
                    sql::execute(db, "DELETE FROM local_episode_addresses WHERE episode_id IN(SELECT id FROM episodes WHERE show_id=?)", {itemId});
                    for (const auto &season : detail->seasons) for (const auto &episode : season.episodes) {
                        if (!episode.seasonNumber || !episode.episodeNumber) continue;
                        const auto found = result.episodes.constFind({*episode.seasonNumber, *episode.episodeNumber});
                        if (found == result.episodes.cend()) continue;
                        if (!found->title.isEmpty())
                            sql::execute(db, "INSERT OR REPLACE INTO local_episode_metadata VALUES(?,?,?,?)",
                                {itemId, *episode.seasonNumber, *episode.episodeNumber, found->title});
                        sql::execute(db, "INSERT OR REPLACE INTO local_episode_addresses VALUES(?,?,?,?)",
                            {episode.identity.itemId, source == "tmdb" ? "tmdb" : source == "tvdb" || (!result.ids.imdb && result.ids.tvdb)
                                ? "tvdb" : "imdb", order, sql::text(found->tvdbId)});
                    }
                    if (keepAliases) segments::copyAliases(db, detail->show, *after);
                }
                if (imageError.isEmpty())
                    sql::execute(db, "INSERT OR REPLACE INTO local_metadata_refresh VALUES(?,?,?)",
                        {itemId, signature, QDateTime::currentSecsSinceEpoch()});
                checkCancelled(cancel);
                transaction.commit();
                ++updated;
                committed = true;
            } catch (const ProviderError &error) {
                const bool missing = dynamic_cast<const MetadataNotFound *>(&error);
                if (missing) ++unmatched; else ++failed;
                if (lookupExample.isEmpty()) lookupExample = item.title + ": " + QString::fromUtf8(error.what());
                sql::Connection connection(m_path);
                const auto statement = isMovie
                    ? "INSERT INTO local_movie_metadata(movie_id,status,error) VALUES(?,?,?) ON CONFLICT(movie_id) DO UPDATE SET status=excluded.status,error=excluded.error"
                    : "INSERT INTO local_show_metadata(show_id,status,error) VALUES(?,?,?) ON CONFLICT(show_id) DO UPDATE SET status=excluded.status,error=excluded.error";
                sql::execute(connection.db, statement, {itemId, missing ? "not_found" : "failed", QString::fromUtf8(error.what())});
                if (error.httpStatus() == 401 || error.httpStatus() == 403 || error.httpStatus() == 429)
                    return report("failed", {}, QString::fromUtf8(error.what()));
            }
            ++current;
            report("running", {}, QString("%1 / %2 %3").arg(current).arg(total).arg(movies ? "movies" : "shows"), {}, committed ? itemId : QString());
            return {};
        };

        for (const auto &item : shows) if (const auto stopped = fetch(item)) return *stopped;
        for (const auto &item : movieList) if (const auto stopped = fetch(item)) return *stopped;
        auto message = QString("Fetch finished: %1 updated, %2 unmatched, %3 failed, %4 artwork errors")
            .arg(updated).arg(unmatched).arg(failed).arg(artworkFailed);
        if (reused) message += QString(", %1 reused").arg(reused);
        if (!lookupExample.isEmpty()) message += '\n' + lookupExample;
        if (!artworkExample.isEmpty()) message += '\n' + artworkExample;
        return report(failed || unmatched || artworkFailed ? "completed_with_errors" : "complete", {}, message);
    } catch (const Cancelled &) {
        return report("canceled", {}, "Metadata fetch stopped");
    } catch (const ProviderError &error) {
        return report("failed", {}, QString::fromUtf8(error.what()));
    } catch (...) {
        return report("failed", {}, "Metadata fetch failed", "Cannot update Local metadata. Check app.db access and retry.");
    }
}
} // namespace introvert::providers
