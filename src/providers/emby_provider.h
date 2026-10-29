#pragma once

#include "contracts.h"

#include <QElapsedTimer>
#include <QJsonObject>
#include <QVariantMap>

namespace introvert::providers {

// Jellyfin shares the source adapter's item/chapter logic but supplies its own
// auth header, routes, first-user discovery and zero-count policy.
class EmbyProvider : public MediaProvider {
public:
    EmbyProvider();
    QList<ShowSummary> listSeries(const ProviderConfig &config, bool includeImages = true,
                                  const Cancellation &cancelled = {}) override;
    std::optional<ShowDetail> getShow(const ProviderConfig &config, const QString &showId,
                                     bool includeImages = true, const Cancellation &cancelled = {}) override;
    QByteArray artwork(const ProviderConfig &config, const QString &path, const Cancellation &cancelled = {}) override;
    QJsonValue request(const ProviderConfig &config, const QString &path, const QVariantMap &parameters = {},
                        const Cancellation &cancelled = {}) override;
    void invalidate() override;
protected:
    explicit EmbyProvider(ProviderId provider);
private:
    QMap<QByteArray, QByteArray> headers(const ProviderConfig &config) const;
    std::optional<QString> user(const ProviderConfig &config, const Cancellation &cancelled);
    QList<QJsonObject> items(const ProviderConfig &config, const QString &path, const QVariantMap &parameters,
                             const Cancellation &cancelled);
    ShowSummary show(const ProviderConfig &config, const QJsonObject &item, bool includeImages) const;
    QString web(const ProviderConfig &config, const QString &key, const std::optional<QString> &server) const;
    ProviderId m_provider;
    QString m_label, m_prefix;
    std::optional<QString> m_userUrl, m_userCredential, m_user;
    QElapsedTimer m_userAge;
};

class JellyfinProvider final : public EmbyProvider {
public:
    JellyfinProvider() : EmbyProvider(ProviderId::Jellyfin) {}
};

} // namespace introvert::providers
