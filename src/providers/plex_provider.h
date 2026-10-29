#pragma once

#include "contracts.h"

#include <QElapsedTimer>
#include <QJsonObject>
#include <QStringList>
#include <QVariantMap>

namespace introvert::providers {

class PlexProvider final : public MediaProvider {
public:
    QList<ShowSummary> listSeries(const ProviderConfig &config, bool includeImages = true,
                                  const Cancellation &cancelled = {}) override;
    std::optional<ShowDetail> getShow(const ProviderConfig &config, const QString &showId,
                                     bool includeImages = true, const Cancellation &cancelled = {}) override;
    QByteArray artwork(const ProviderConfig &config, const QString &path, const Cancellation &cancelled = {}) override;
    QJsonValue request(const ProviderConfig &config, const QString &path, const QVariantMap &parameters = {},
                        const Cancellation &cancelled = {}) override;
    void invalidate() override;
private:
    void check(const ProviderConfig &config, const Cancellation &cancelled) const;
    void context(const ProviderConfig &config, const Cancellation &cancelled);
    QList<QJsonObject> items(const ProviderConfig &config, const QString &path, const QVariantMap &parameters,
                             const Cancellation &cancelled);
    ShowSummary show(const ProviderConfig &config, const QJsonObject &item, bool includeImages) const;
    QString web(const ProviderConfig &config, const QString &key) const;
    std::optional<QString> m_contextUrl, m_contextCredential, m_machine;
    QStringList m_sections;
    QElapsedTimer m_contextAge;
};

} // namespace introvert::providers
