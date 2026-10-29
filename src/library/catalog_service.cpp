#include "catalog_service.h"
#include "providers/plex_provider.h"
#include "providers/emby_provider.h"
#include "providers/local_provider.h"
#include "segments/catalog_segments.h"
#include <QElapsedTimer>
#include <QUrl>

namespace introvert::library {
using namespace providers;

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

CatalogService::CatalogService(SettingsStore store, MediaCache &cache, ProviderMap overrides)
    : m_store(std::move(store)), m_cache(cache), m_providers(std::move(overrides))
{
    if (!m_providers.count(ProviderId::Plex)) m_providers[ProviderId::Plex] = std::make_unique<PlexProvider>();
    if (!m_providers.count(ProviderId::Emby)) m_providers[ProviderId::Emby] = std::make_unique<EmbyProvider>();
    if (!m_providers.count(ProviderId::Jellyfin)) m_providers[ProviderId::Jellyfin] = std::make_unique<JellyfinProvider>();
    if (!m_providers.count(ProviderId::Local)) m_providers[ProviderId::Local] = std::make_unique<LocalProvider>(m_store.databasePath());
}

ProviderConfig CatalogService::config(const ProviderSettings &saved, const Cancellation &cancel) const
{
    checkCancelled(cancel);
    ProviderConfig result(providerId(saved.provider));
    result.remoteUrl = saved.remoteUrl;
    result.localRoot = saved.localRoot;
    result.fetchImages = saved.fetchImages;
    result.timeoutSeconds = saved.timeoutSeconds;
    if (result.provider != ProviderId::Local) {
        result.credential = m_store.credential("media:" + saved.provider);
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
    auto destination = force ? std::optional<QString>() : m_cache.image(scope, *path);
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
    return {{"id", show.identity.itemId}, {"title", show.title}, {"year", count(show.year)},
        {"seasons", count(show.seasonCount)}, {"episodes", count(show.episodeCount)},
        {"serverId", text(show.identity.serverId)}, {"provider", providerName(show.identity.provider)},
        {"posterUrl", images ? artwork(config, show.posterUrl, cancel) : QString()},
        {"logoUrl", images ? artwork(config, show.logoUrl, cancel) : QString()},
        {"metadataStatus", show.localMetadata ? text(show.localMetadata->metadataStatus) : QString()},
        {"metadataError", show.localMetadata ? text(show.localMetadata->metadataError) : QString()},
        {"externalIds", QVariantMap{{"tvmaze", text(show.externalIds.tvmaze)}, {"imdb", text(show.externalIds.imdb)},
                                   {"tmdb", text(show.externalIds.tmdb)}, {"tvdb", text(show.externalIds.tvdb)}}}};
}

CatalogResult CatalogService::browse(const ProviderConfig &config, const QString &listing, bool force, bool clearLocal,
                                    const QVariantList &previous, const Cancellation &cancel, const Progress &progress)
{
    checkCancelled(cancel);
    auto &provider = *m_providers.at(config.provider);
    const bool remote = config.provider != ProviderId::Local;
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
                const auto loaded = detail(config, listing, show.identity.itemId, cancel);
                checkCancelled(cancel);
                segments::loadCatalogSegments(m_store.databasePath(), loaded);
            } catch (const ProviderError &) { ++unavailable; }
        }
        if (unavailable) result.message = QString("Library loaded. Existing timestamps could not be checked for %1 show(s); open the show or refresh to retry.").arg(unavailable);
    };
    const auto fresh = remote && !force ? m_cache.series(scope) : std::optional<QList<ShowSummary>>();
    if (fresh) {
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
        provider.invalidate();
        if (clearLocal && !remote) {
            auto images = config;
            for (bool enabled : {false, true}) { images.fetchImages = enabled; m_cache.clearImages(m_cache.scope(images, "images")); }
            // Default LocalProvider owns the catalog. Overrides may supply a
            // fixture provider, but cannot silently pretend to clear that catalog.
            auto *local = dynamic_cast<LocalProvider *>(&provider);
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
        result.message = QString::fromUtf8(error.what()) + " Showing cached library.";
        display(*stale);
        return result;
    }
    checkCancelled(cancel);
    if (remote) {
        m_cache.putSeries(scope, values);
        if (force) m_cache.invalidateDetails(scope);
    }
    result.cached = false; result.message.clear(); display(values); import(values);
    return result;
}

ShowDetail CatalogService::detail(const ProviderConfig &config, const QString &listing, const QString &id, const Cancellation &cancel)
{
    checkCancelled(cancel);
    const bool remote = config.provider != ProviderId::Local;
    const auto scope = remote ? m_cache.scope(config, listing) : QString();
    auto result = remote ? m_cache.detail(scope, id) : std::optional<ShowDetail>();
    if (!result) {
        {
            const std::lock_guard<std::mutex> lock(m_providerMutex);
            checkCancelled(cancel);
            result = m_providers.at(config.provider)->getShow(config, id, listing != "basic", cancel);
        }
        if (!result) throw ProviderError("This show is no longer available.");
        checkCancelled(cancel);
        if (remote) m_cache.putDetail(scope, *result, id);
    }
    return *result;
}

CatalogResult CatalogService::rebuild(const ProviderConfig &config, const QString &listing,
                                     const Cancellation &cancel, const std::function<void(double)> &progress)
{
    checkCancelled(cancel);
    auto &provider = *m_providers.at(config.provider);
    QList<ShowSummary> values;
    {
        const std::lock_guard<std::mutex> lock(m_providerMutex);
        checkCancelled(cancel);
        provider.invalidate();
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
            detail = provider.getShow(config, show.identity.itemId, true, cancel);
        }
        if (!detail) throw ProviderError("A show changed during the rebuild. Try rebuilding the cache again.");
        for (const auto &path : {show.posterUrl, show.logoUrl, detail->show.posterUrl, detail->show.logoUrl})
            if (path && !path->isEmpty() && !images.contains(*path)) images[*path] = artwork(config, path, cancel, true);
        checkCancelled(cancel);
        segments::loadCatalogSegments(m_store.databasePath(), *detail);
        requireStored(m_cache.putDetail(full, *detail, show.identity.itemId));
        detail->show.posterUrl.reset(); detail->show.logoUrl.reset();
        requireStored(m_cache.putDetail(basic, *detail, show.identity.itemId));
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
    requireStored(m_cache.putSeries(full, values));
    requireStored(m_cache.putSeries(basic, withoutImages));
    return result;
}

} // namespace introvert::library
