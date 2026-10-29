#pragma once

#include "local_provider.h"
#include "metadata_client.h"
#include "core/settings_store.h"
#include "library/media_cache.h"
#include <functional>

namespace introvert::providers {

// Compose after the shared SettingsStore is initialized. Operations are
// scheduled on an owning worker, with an independent LocalProvider instance.
class MetadataService final {
public:
    using Progress = std::function<void(const QVariantMap &)>;
    MetadataService(QString databasePath, library::MediaCache &cache, MetadataClient client = MetadataClient());
    QVariantMap status(const QString &root) const;
    QVariantMap record(const QString &root, const QString &state, qint64 current, qint64 total,
                       const QString &title = {}, const QString &message = {}, const QString &error = {}) const;
    QVariantMap recover(const QString &root) const;
    QVariantMap run(const ProviderConfig &config, const Cancellation &cancelled, const Progress &progress);
private:
    QString m_path;
    LocalProvider m_provider;
    library::MediaCache &m_cache;
    MetadataClient m_client;
    SettingsStore m_settings;
};

} // namespace introvert::providers
