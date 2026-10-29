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
};

// Synchronous worker-owned coordinator. Settings and QML never perform HTTP or
// filesystem scans. Provider calls are serialized, allowing foreground detail
// requests between background artwork/import operations.
class CatalogService final {
public:
    using Progress = std::function<void(const CatalogResult &)>;
    CatalogService(SettingsStore store, MediaCache &cache, ProviderMap overrides = {});
    providers::ProviderConfig config(const ProviderSettings &saved, const providers::Cancellation &cancel) const;
    CatalogResult browse(const providers::ProviderConfig &config, const QString &listing, bool force, bool clearLocal,
                         const QVariantList &previous, const providers::Cancellation &cancel, const Progress &progress);
    CatalogResult rebuild(const providers::ProviderConfig &config, const QString &listing,
                          const providers::Cancellation &cancel, const std::function<void(double)> &progress);
    providers::ShowDetail detail(const providers::ProviderConfig &config, const QString &listing, const QString &id,
                                 const providers::Cancellation &cancel);
    QVariantMap displayShow(const providers::ProviderConfig &config, const providers::ShowSummary &show,
                            const providers::Cancellation &cancel, bool images = true);
private:
    QString artwork(const providers::ProviderConfig &config, const std::optional<QString> &path,
                    const providers::Cancellation &cancel, bool force = false);
    SettingsStore m_store;
    MediaCache &m_cache;
    ProviderMap m_providers;
    std::mutex m_providerMutex;
};

QString providerLabel(providers::ProviderId id);

} // namespace introvert::library
