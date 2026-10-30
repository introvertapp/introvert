#pragma once

#include "contracts.h"
#include <QSqlDatabase>
#include <functional>

namespace introvert::providers {

class LocalProvider final : public MediaProvider {
public:
    using Inspector = std::function<bool(Episode &, const Cancellation &)>;
    explicit LocalProvider(QString databasePath, Inspector inspector = {});
    QString databasePath() const { return m_path; }
    void initializeCatalog();
    bool hasCatalog(const ProviderConfig &config, const Cancellation &cancelled = {});
    void refreshCatalog(const ProviderConfig &config, const Cancellation &cancelled = {});
    static QString rootPath(const ProviderConfig &config, const Cancellation &cancelled = {});
    QList<ShowSummary> listSeries(const ProviderConfig &config, bool includeImages = true,
                                  const Cancellation &cancelled = {}) override;
    std::optional<ShowDetail> getShow(const ProviderConfig &config, const QString &showId,
                                     bool includeImages = true, const Cancellation &cancelled = {}) override;
    std::optional<ShowDetail> inspectShow(const ProviderConfig &config, const QString &showId,
                                         bool includeImages = true, const Cancellation &cancelled = {});
    QByteArray artwork(const ProviderConfig &config, const QString &path, const Cancellation &cancelled = {}) override;
    QJsonValue request(const ProviderConfig &config, const QString &path, const QVariantMap &parameters = {},
                        const Cancellation &cancelled = {}) override;
    void invalidate() override { m_rescan = true; }
    void clear(const ProviderConfig &config, const Cancellation &cancelled = {});
private:
    void initialize(QSqlDatabase &db);
    void scan(QSqlDatabase &db, const QString &root, const Cancellation &cancelled);
    std::optional<ShowDetail> readShow(const ProviderConfig &config, const QString &showId,
                                      bool includeImages, const Cancellation &cancelled, bool inspectFiles);
    ShowSummary show(QSqlDatabase &db, const QString &id, const QString &title, const QVariant &year,
                      const QString &path, bool images);
    QString m_path;
    Inspector m_inspector;
    bool m_rescan = false;
};

} // namespace introvert::providers
