#include "catalog_service.h"
#include "providers/plex_provider.h"
#include "providers/emby_provider.h"
#include "providers/local_provider.h"
#include "segments/catalog_segments.h"
#include <QElapsedTimer>
#include <QUrl>

namespace introvert::library {
using namespace providers;
namespace {

template<typename Summary>
QVariantMap titleRow(const Summary &title)
{
    const auto text = [](const std::optional<QString> &value) { return value.value_or(QString()); };
    return {{"id", title.identity.itemId}, {"title", title.title}, {"year", title.year ? QVariant(*title.year) : QVariant("—")},
        {"serverId", text(title.identity.serverId)}, {"provider", providerName(title.identity.provider)},
        {"externalIds", QVariantMap{{"tvmaze", text(title.externalIds.tvmaze)}, {"imdb", text(title.externalIds.imdb)},
                                   {"tmdb", text(title.externalIds.tmdb)}, {"tvdb", text(title.externalIds.tvdb)}}}};
}

} // namespace

QString providerLabel(ProviderId id)
{
    switch (id) {
    case ProviderId::Plex: return "Plex";
    case ProviderId::Emby: return "Emby";
    case ProviderId::Jellyfin: return "Jellyfin";
    case ProviderId::Local: return "Local";
    }
    return {};
}

CatalogService::CatalogService(SettingsStore store, MediaCache &cache, ProviderMap overrides, Diagnostic diagnostic)
    : m_store(std::move(store)), m_cache(cache), m_providers(std::move(overrides)), m_diagnostic(std::move(diagnostic))
{
    if (!m_providers.count(ProviderId::Plex)) m_providers[ProviderId::Plex] = std::make_unique<PlexProvider>();
    if (!m_providers.count(ProviderId::Emby)) m_providers[ProviderId::Emby] = std::make_unique<EmbyProvider>();
    if (!m_providers.count(ProviderId::Jellyfin)) m_providers[ProviderId::Jellyfin] = std::make_unique<JellyfinProvider>();
    if (!m_providers.count(ProviderId::Local)) m_providers[ProviderId::Local] = std::make_unique<LocalProvider>(m_store.databasePath());
}

void CatalogService::log(const QString &level, const QString &message) const
{
    if (m_diagnostic) m_diagnostic(level, message);
}

QList<ShowSummary> CatalogService::series(const ProviderConfig &config, const Cancellation &cancel)
{
    const std::lock_guard<std::mutex> lock(m_providerMutex);
    checkCancelled(cancel);
    return m_providers.at(config.provider)->listSeries(config, false, cancel);
}

ProviderConfig CatalogService::config(const ProviderSettings &saved, const Cancellation &cancel) const
{
    checkCancelled(cancel);
    ProviderConfig result(providerId(saved.provider));
    result.remoteUrl = saved.remoteUrl;
    result.localRoot = saved.localRoot;
    result.fetchImages = saved.fetchImages;
    result.timeoutSeconds = saved.timeoutSeconds;
    result.importSegments = saved.importSegments;
    if(result.provider==ProviderId::Local && result.localRoot && !result.localRoot->isEmpty())
        result.fetchImages=m_store.loadLocalMetadata(LocalProvider::rootPath(result)).value("artwork").toString()!="off";
    if (result.provider != ProviderId::Local) {
        result.credential = m_store.providerCredential(saved);
        if (!result.credential || result.credential->isEmpty())
            throw ProviderError("Enter your " + providerLabel(result.provider) + " API key/token in Settings → Media.");
    }
    return result;
}

QString CatalogService::artwork(const ProviderConfig &config, const std::optional<QString> &path,
                               const Cancellation &cancel, bool force)
{
    if (!path || path->isEmpty()) return {};
    const auto scope = m_cache.scope(config, "images");
    auto destination = force ? std::optional<QString>() : m_cache.image(scope, *path, config.provider==ProviderId::Local);
    if(config.provider==ProviderId::Local)return destination?QUrl::fromLocalFile(*destination).toString():QString();
    if (!destination) {
        try {
            QByteArray bytes;
            {
                const std::lock_guard<std::mutex> lock(m_providerMutex);
                checkCancelled(cancel);
                bytes = m_providers.at(config.provider)->artwork(config, *path, cancel);
            }
            checkCancelled(cancel);
            destination = m_cache.putImage(scope, *path, bytes);
            if (force && !destination) throw ProviderError("Cannot save artwork to the image cache. Check data directory access and available space.");
        } catch (const ProviderError &error) {
            if (force) {
                if (error.httpStatus() == 404) { m_cache.discardImage(scope, *path); return {}; }
                throw;
            }
            destination = m_cache.image(scope, *path, true);
        }
    }
    return destination ? QUrl::fromLocalFile(*destination).toString() : QString();
}

QVariantMap CatalogService::displayShow(const ProviderConfig &config, const ShowSummary &show,
                                      const Cancellation &cancel, bool images)
{
    const auto count = [](const std::optional<qint64> &value) -> QVariant { return value ? QVariant(*value) : QVariant("—"); };
    const auto text = [](const std::optional<QString> &value) { return value.value_or(QString()); };
    auto row = titleRow(show);
    row["seasons"] = count(show.seasonCount);
    row["episodes"] = count(show.episodeCount);
    row["posterUrl"] = images ? artwork(config, show.posterUrl, cancel) : QString();
    row["logoUrl"] = images ? artwork(config, show.logoUrl, cancel) : QString();
    row["metadataStatus"] = show.localMetadata ? text(show.localMetadata->metadataStatus) : QString();
    row["metadataError"] = show.localMetadata ? text(show.localMetadata->metadataError) : QString();
    return row;
}

QVariantMap CatalogService::displayMovie(const ProviderConfig &config, const MovieSummary &movie,
                                       const Cancellation &cancel, bool images)
{
    auto row = titleRow(movie);
    row["durationMs"] = movie.durationMs ? QVariant(*movie.durationMs) : QVariant();
    row["posterUrl"] = images ? artwork(config, movie.posterUrl, cancel) : QString();
    row["logoUrl"] = images ? artwork(config, movie.logoUrl, cancel) : QString();
    return row;
}

QList<MovieSummary> CatalogService::listMovies(const ProviderConfig &config, bool images, bool refresh, const Cancellation &cancel)
{
    const std::lock_guard<std::mutex> lock(m_providerMutex);
    checkCancelled(cancel);
    auto &provider = *m_providers.at(config.provider);
    if (refresh) provider.invalidate();
    return provider.listMovies(config, images, cancel);
}

CatalogResult CatalogService::browseMovies(const ProviderConfig &config, const QString &listing, bool force,
                                           const QVariantList &previous, const Cancellation &cancel, const Progress &progress)
{
    checkCancelled(cancel);
    const auto scope = m_cache.scope(config, listing);
    const bool images = listing != "basic";
    auto movies = force ? std::optional<QList<MovieSummary>>() : m_cache.movies(scope);
    CatalogResult result;
    if (movies) {
        result.cached = true;
        result.message = "Using cached " + providerLabel(config.provider) + " movies.";
    } else {
        try {
            movies = listMovies(config, images, force, cancel);
        } catch (const ProviderError &error) {
            checkCancelled(cancel);
            movies = m_cache.movies(scope, true);
            if (!movies) throw;
            result.cached = true;
            result.message = QString::fromUtf8(error.what()) + " Showing cached movies.";
        }
        checkCancelled(cancel);
        if (!result.cached) {
            m_cache.putMovies(scope, *movies);
            if (force) m_cache.invalidateMovieDetails(scope);
        }
    }
    QHash<QString, QVariant> posters;
    if (images) for (const auto &item : previous) {
        const auto row = item.toMap();
        posters.insert(row.value("id").toString(), row.value("posterUrl"));
    }
    for (const auto &movie : *movies) {
        checkCancelled(cancel);
        auto row = displayMovie(config, movie, cancel, false);
        if (images && movie.posterUrl) row["posterUrl"] = posters.value(movie.identity.itemId);
        result.rows.append(row);
    }
    progress(result);
    if (images) {
        QElapsedTimer published;
        published.start();
        for (qsizetype index = 0; index < movies->size(); ++index) {
            checkCancelled(cancel);
            auto row = result.rows.at(index).toMap();
            row["posterUrl"] = artwork(config, movies->at(index).posterUrl, cancel);
            result.rows[index] = std::move(row);
            if (published.elapsed() >= 75 || index + 1 == movies->size()) {
                progress(result);
                published.restart();
            }
        }
    }
    log("info", QString("%1 movie library loaded: %2 movie(s).").arg(providerLabel(config.provider)).arg(movies->size()));
    return result;
}

MovieDetail CatalogService::movieDetail(const ProviderConfig &config, const QString &listing, const QString &id,
                                        const Cancellation &cancel)
{
    checkCancelled(cancel);
    const bool images = listing != "basic";
    const auto scope = m_cache.scope(config, listing);
    auto detail = m_cache.movie(scope, id);
    if (!detail) {
        {
            const std::lock_guard<std::mutex> lock(m_providerMutex);
            checkCancelled(cancel);
            detail = m_providers.at(config.provider)->getMovie(config, id, images, cancel);
        }
        if (!detail) throw ProviderError("This movie is no longer available.");
        checkCancelled(cancel);
        m_cache.putMovie(scope, *detail);
    }
    return *detail;
}

CatalogResult CatalogService::rebuildMovies(const ProviderConfig &config, const QString &listing,
                                            const Cancellation &cancel, const std::function<void(double)> &progress)
{
    const auto movies = listMovies(config, true, true, cancel);
    const auto full = m_cache.scope(config, "full"), basic = m_cache.scope(config, "basic");
    const auto total = double(movies.size() + 2);
    progress(1 / total);
    CatalogResult result;
    QList<MovieSummary> plain;
    QMap<QString, QString> images;
    for (qsizetype index = 0; index < movies.size(); ++index) {
        checkCancelled(cancel);
        const auto &movie = movies.at(index);
        const auto &path = movie.posterUrl;
        if (path && !path->isEmpty() && !images.contains(*path)) images[*path] = artwork(config, path, cancel, true);
        auto row = displayMovie(config, movie, cancel, false);
        if (listing != "basic")
            row["posterUrl"] = images.value(movie.posterUrl.value_or(QString()));
        result.rows.append(row);
        auto withoutImages = movie;
        withoutImages.posterUrl.reset();
        withoutImages.logoUrl.reset();
        plain.append(std::move(withoutImages));
        progress((index + 2) / total);
    }
    checkCancelled(cancel);
    if (!m_cache.putMovies(full, movies) || !m_cache.putMovies(basic, plain))
        throw ProviderError("Cannot save metadata to app.db. Check database access and available space.");
    m_cache.invalidateMovieDetails(full);
    m_cache.invalidateMovieDetails(basic);
    return result;
}

CatalogResult CatalogService::browse(const ProviderConfig &config, const QString &listing, bool force, bool clearLocal,
                                    const QVariantList &previous, const Cancellation &cancel, const Progress &progress)
{
    checkCancelled(cancel);
    log("info", providerLabel(config.provider) + " metadata refresh started.");
    auto &provider = *m_providers.at(config.provider);
    const bool remote = config.provider != ProviderId::Local;
    auto *local=dynamic_cast<LocalProvider *>(&provider);
    bool localCached=false;
    const auto scope = remote ? m_cache.scope(config, listing) : QString();
    CatalogResult result;
    QMap<QString, QVariantMap> old;
    for (const auto &row : previous) old[row.toMap().value("id").toString()] = row.toMap();
    const auto display = [&](const QList<ShowSummary> &values) {
        result.rows.clear();
        for (const auto &show : values) {
            checkCancelled(cancel);
            auto row = displayShow(config, show, cancel, false);
            if (listing != "basic" && (remote || config.fetchImages)) {
                const auto prior = old.value(show.identity.itemId);
                if (show.posterUrl) row["posterUrl"] = prior.value("posterUrl", QString());
                if (show.logoUrl) row["logoUrl"] = prior.value("logoUrl", QString());
            }
            result.rows.append(row);
        }
        progress(result);
        QElapsedTimer published; published.start();
        for (qsizetype index = 0; index < values.size(); ++index) {
            checkCancelled(cancel);
            result.rows[index] = displayShow(config, values[index], cancel);
            if (published.elapsed() >= 75 || index + 1 == values.size()) { progress(result); published.restart(); }
        }
    };
    const auto import = [&](const QList<ShowSummary> &values) {
        int unavailable = 0;
        for (const auto &show : values) {
            checkCancelled(cancel);
            try {
                auto loaded = [&] {
                    if(local) {
                        const std::lock_guard<std::mutex> lock(m_providerMutex);checkCancelled(cancel);
                        const auto value=local->inspectShow(config,show.identity.itemId,listing!="basic",cancel);
                        if(!value)throw ProviderError("This show is no longer available.");return *value;
                    }
                    return detail(config,listing,show.identity.itemId,cancel);
                }();
                checkCancelled(cancel);
                const auto imported=segments::loadCatalogSegments(m_store.databasePath(), loaded,
                    config.provider==ProviderId::Local?config.localRoot.value_or(QString()):config.remoteUrl.value_or(QString()));
                if(!imported.deferredSeasons.isEmpty())log("info","Timestamp import deferred for a season reserved by detection. Refresh after detection finishes to retry.");
            } catch (const ProviderError &) {
                checkCancelled(cancel); ++unavailable;
                log("warning", providerLabel(config.provider) + " timestamp import could not load a show. Open the show or refresh to retry.");
            }
        }
        log("info", QString("%1 metadata import checked %2 show(s); %3 unavailable.").arg(providerLabel(config.provider)).arg(values.size()).arg(unavailable));
        if (unavailable) result.message = QString("Library loaded. Existing timestamps could not be checked for %1 show(s); open the show or refresh to retry.").arg(unavailable);
    };
    const auto fresh = remote && !force ? m_cache.series(scope) : std::optional<QList<ShowSummary>>();
    if (fresh) {
        log("info", QString("%1 metadata loaded from cache: %2 show(s).").arg(providerLabel(config.provider)).arg(fresh->size()));
        result.cached = true;
        result.message = "Using cached " + providerLabel(config.provider) + " library.";
        display(*fresh);
        import(*fresh);
        return result;
    }
    const auto stale = remote ? m_cache.series(scope, {}, MediaCache::seriesTtl, true) : std::optional<QList<ShowSummary>>();
    if (stale) {
        result.cached = true; result.message = "Checking for library changes…"; display(*stale);
    }
    {
        const std::lock_guard<std::mutex> lock(m_providerMutex);
        checkCancelled(cancel);
        localCached=local && !force && !clearLocal && local->hasCatalog(config,cancel);
        if(remote || force || clearLocal)provider.invalidate();
        if (clearLocal && !remote) {
            auto images = config;
            for (bool enabled : {false, true}) { images.fetchImages = enabled; m_cache.clearImages(m_cache.scope(images, "images")); }
            // Default LocalProvider owns the catalog. Overrides may supply a
            // fixture provider, but cannot silently pretend to clear that catalog.
            if (!local) throw ProviderError("Cannot clear the Local media catalog.");
            local->clear(config, cancel);
        }
    }
    QList<ShowSummary> values;
    try {
        const std::lock_guard<std::mutex> lock(m_providerMutex);
        checkCancelled(cancel);
        values = provider.listSeries(config, listing != "basic", cancel);
    }
    catch (const ProviderError &error) {
        if (!stale) throw;
        result.cached = true;
        log("warning", providerLabel(config.provider) + " metadata refresh failed; using cached metadata. Check the server connection and retry.");
        result.message = QString::fromUtf8(error.what()) + " Showing cached library.";
        display(*stale);
        return result;
    }
    checkCancelled(cancel);
    if (remote) {
        m_cache.putSeries(scope, values);
        if (force) m_cache.invalidateDetails(scope);
    }
    result.cached = localCached; result.message=localCached?"Using cached Local library.":QString();
    display(values);
    log("info", QString("%1 metadata refresh finished: %2 show(s).").arg(providerLabel(config.provider)).arg(values.size()));
    if(!localCached)import(values);
    return result;
}

ShowDetail CatalogService::detail(const ProviderConfig &config, const QString &listing, const QString &id, const Cancellation &cancel)
{
    checkCancelled(cancel);
    const bool remote = config.provider != ProviderId::Local;
    const auto scope = remote ? m_cache.scope(config, listing) : QString();
    auto result = remote ? m_cache.detail(scope, id) : std::optional<ShowDetail>();
    if(result && remote && !result->hasEpisodeAddressing)result.reset();
    if (!result) {
        {
            const std::lock_guard<std::mutex> lock(m_providerMutex);
            checkCancelled(cancel);
            result = m_providers.at(config.provider)->getShow(config, id, listing != "basic", cancel);
        }
        if (!result) {
            log("warning", providerLabel(config.provider) + " show metadata is no longer available.");
            throw ProviderError("This show is no longer available.");
        }
        log("info", providerLabel(config.provider) + " show metadata imported: " + result->show.title + ".");
        checkCancelled(cancel);
        if (remote) m_cache.putDetail(scope, *result, id);
    }
    if (remote && !config.importSegments)
        for (auto &season : result->seasons) for (auto &episode : season.episodes) episode.importedSegments.clear();
    return *result;
}

CatalogResult CatalogService::rebuild(const ProviderConfig &config, const QString &listing,
                                     const Cancellation &cancel, const std::function<void(double)> &progress)
{
    checkCancelled(cancel);
    log("info", providerLabel(config.provider) + " metadata refresh started.");
    auto &provider = *m_providers.at(config.provider);
    const bool remote=config.provider!=ProviderId::Local;
    auto *local=dynamic_cast<LocalProvider *>(&provider);
    QList<ShowSummary> values;
    {
        const std::lock_guard<std::mutex> lock(m_providerMutex);
        checkCancelled(cancel);
        if(local)local->refreshCatalog(config,cancel);else provider.invalidate();
        values = provider.listSeries(config, true, cancel);
    }
    checkCancelled(cancel);
    const auto full = m_cache.scope(config, "full"), basic = m_cache.scope(config, "basic");
    const auto total = double(values.size() + 2);
    progress(1 / total);
    CatalogResult result;
    QMap<QString, QString> images;
    QList<ShowSummary> withoutImages;
    const auto requireStored = [](bool saved) {
        if (!saved) throw ProviderError("Cannot save metadata to app.db. Check database access and available space.");
    };
    for (qsizetype index = 0; index < values.size(); ++index) {
        checkCancelled(cancel);
        const auto &show = values[index];
        std::optional<ShowDetail> detail;
        {
            const std::lock_guard<std::mutex> lock(m_providerMutex);
            checkCancelled(cancel);
            detail = local ? local->inspectShow(config,show.identity.itemId,true,cancel) : provider.getShow(config, show.identity.itemId, true, cancel);
        }
        if (!detail) throw ProviderError("A show changed during the rebuild. Try rebuilding the cache again.");
        for (const auto &path : {show.posterUrl, show.logoUrl, detail->show.posterUrl, detail->show.logoUrl})
            if (path && !path->isEmpty() && !images.contains(*path)) images[*path] = artwork(config, path, cancel, remote);
        checkCancelled(cancel);
        if(remote)requireStored(m_cache.putDetail(full, *detail, show.identity.itemId));
        detail->show.posterUrl.reset(); detail->show.logoUrl.reset();
        if(remote)requireStored(m_cache.putDetail(basic, *detail, show.identity.itemId));
        if (remote && !config.importSegments)
            for (auto &season : detail->seasons) for (auto &episode : season.episodes) episode.importedSegments.clear();
        const auto imported=segments::loadCatalogSegments(m_store.databasePath(), *detail,
            config.provider==ProviderId::Local?config.localRoot.value_or(QString()):config.remoteUrl.value_or(QString()));
        if(!imported.deferredSeasons.isEmpty())log("info","Timestamp import deferred for a season reserved by detection. Refresh after detection finishes to retry.");
        auto row = displayShow(config, show, cancel, false);
        if (listing != "basic") {
            row["posterUrl"] = images.value(show.posterUrl.value_or(QString()));
            row["logoUrl"] = images.value(show.logoUrl.value_or(QString()));
        }
        result.rows.append(row);
        auto plain = show; plain.posterUrl.reset(); plain.logoUrl.reset(); withoutImages.append(plain);
        progress((index + 2) / total);
    }
    checkCancelled(cancel);
    if(remote) {
        requireStored(m_cache.putSeries(full, values));
        requireStored(m_cache.putSeries(basic, withoutImages));
    }
    log("info", QString("%1 metadata rebuild finished: %2 show(s).").arg(providerLabel(config.provider)).arg(values.size()));
    return result;
}

} // namespace introvert::library
