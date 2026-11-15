#pragma once

#include "contracts.h"

#include <QElapsedTimer>
#include <QJsonObject>
#include <QMap>

namespace introvert::providers {

// Silo's official native v2 API. Instances, like the other source adapters,
// are used serially on the owning library/stream-resolution worker.
class SiloProvider final : public MediaProvider {
public:
    QList<ShowSummary> listSeries(const ProviderConfig &, bool = true, const Cancellation & = {}) override;
    std::optional<ShowDetail> getShow(const ProviderConfig &, const QString &, bool = true, const Cancellation & = {}) override;
    QList<MovieSummary> listMovies(const ProviderConfig &, bool = true, const Cancellation & = {}) override;
    std::optional<MovieDetail> getMovie(const ProviderConfig &, const QString &, bool = true, const Cancellation & = {}) override;
    QByteArray artwork(const ProviderConfig &, const QString &, const Cancellation & = {}) override;
    QJsonValue request(const ProviderConfig &, const QString &, const QVariantMap & = {}, const Cancellation & = {}) override;
    void invalidate() override;

    QMap<QByteArray, QByteArray> accessHeaders(const ProviderConfig &, const Cancellation & = {});
    QString mediaFile(const ProviderConfig &, const QString &, const Cancellation & = {});
private:
    QList<QJsonObject> items(const ProviderConfig &, const QString &, const QVariantMap &, const Cancellation &);
    QJsonObject item(const ProviderConfig &, const QString &, const Cancellation &);
    template<typename Summary> Summary title(const QJsonObject &, bool) const;
    ShowSummary show(const ProviderConfig &, const QJsonObject &, bool) const;
    QElapsedTimer m_profileAge;
    std::optional<QString> m_url, m_credential;
    QString m_profile;
};

} // namespace introvert::providers
