#pragma once

#include "media_cache.h"
#include "core/settings_store.h"
#include <functional>
#include <map>
#include <mutex>

namespace introvert::library {

using ProviderMap = std::map<providers::ProviderId, std::unique_ptr<providers::MediaProvider>>;
struct CatalogResult {
    QVariantList rows;
    QString message;
    bool cached = false;
    bool warning = false;
};

// Synchronous worker-owned coordinator. Settings and QML never perform HTTP or
// filesystem scans. Provider calls are serialized, allowing foreground detail
// requests between background artwork/import operations.
class CatalogService final {
public:
    using Progress = std::function<void(const CatalogResult &)>;
    using Diagnostic = std::function<void(const QString &, const QString &)>;
    CatalogService(SettingsStore store, MediaCache &cache, ProviderMap overrides = {}, Diagnostic diagnostic = {});
    QList<providers::ShowSummary> series(const providers::ProviderConfig &config, const providers::Cancellation &cancel);
    providers::ProviderConfig config(const ProviderSettings &saved, const providers::Cancellation &cancel) const;
    CatalogResult browse(const providers::ProviderConfig &config, const QString &listing, bool force, bool clearLocal,
                         const QVariantList &previous, const providers::Cancellation &cancel, const Progress &progress);
    CatalogResult rebuild(const providers::ProviderConfig &config, const QString &listing,
                          const providers::Cancellation &cancel, const std::function<void(double)> &progress);
    providers::ShowDetail detail(const providers::ProviderConfig &config, const QString &listing, const QString &id,
                                 const providers::Cancellation &cancel);
    QVariantMap displayShow(const providers::ProviderConfig &config, const providers::ShowSummary &show,
                            const providers::Cancellation &cancel, bool images = true);
    CatalogResult browseMovies(const providers::ProviderConfig &config, const QString &listing, bool force,
                               const QVariantList &previous, const providers::Cancellation &cancel, const Progress &progress);
    CatalogResult rebuildMovies(const providers::ProviderConfig &config, const QString &listing,
                                const providers::Cancellation &cancel, const std::function<void(double)> &progress);
    providers::MovieDetail movieDetail(const providers::ProviderConfig &config, const QString &listing, const QString &id,
                                       const providers::Cancellation &cancel, bool inspectMedia = true);
    void clearLocalMovieMetadata(const providers::ProviderConfig &config, const providers::Cancellation &cancel);
    QVariantMap displayMovie(const providers::ProviderConfig &config, const providers::MovieSummary &movie,
                             const providers::Cancellation &cancel, bool images);
private:
    CatalogResult localMovies(const providers::ProviderConfig &config, bool force, bool images, const providers::Cancellation &cancel,
                              const std::function<void(double)> &progress);
    CatalogResult localTv(const providers::ProviderConfig &config, const providers::Cancellation &cancel,
                          const std::function<CatalogResult(const providers::ProviderConfig &, qsizetype)> &load);
    QList<providers::MovieSummary> listMovies(const providers::ProviderConfig &config, bool images, bool refresh,
                                              const providers::Cancellation &cancel);
    QString artwork(const providers::ProviderConfig &config, const std::optional<QString> &path,
                    const providers::Cancellation &cancel, bool force = false);
    SettingsStore m_store;
    MediaCache &m_cache;
    ProviderMap m_providers;
    std::mutex m_providerMutex;
    Diagnostic m_diagnostic;
    void log(const QString &level, const QString &message) const;
};

QString providerLabel(providers::ProviderId id);

} // namespace introvert::library
