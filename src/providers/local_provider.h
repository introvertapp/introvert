#pragma once

#include "contracts.h"
#include <QSqlDatabase>

namespace introvert::providers {

class LocalProvider final : public MediaProvider {
public:
    explicit LocalProvider(QString databasePath);
    QString databasePath() const { return m_path; }
    void initializeCatalog();
    static QString rootPath(const ProviderConfig &config, const Cancellation &cancelled = {});
    QList<ShowSummary> listSeries(const ProviderConfig &config, bool includeImages = true,
                                  const Cancellation &cancelled = {}) override;
    std::optional<ShowDetail> getShow(const ProviderConfig &config, const QString &showId,
                                     bool includeImages = true, const Cancellation &cancelled = {}) override;
    QByteArray artwork(const ProviderConfig &config, const QString &path, const Cancellation &cancelled = {}) override;
    QJsonValue request(const ProviderConfig &config, const QString &path, const QVariantMap &parameters = {},
                        const Cancellation &cancelled = {}) override;
    void invalidate() override { m_rescan = true; }
    void clear(const ProviderConfig &config, const Cancellation &cancelled = {});
private:
    void initialize(QSqlDatabase &db);
    void scan(QSqlDatabase &db, const QString &root, const Cancellation &cancelled);
    ShowSummary show(QSqlDatabase &db, const QString &id, const QString &title, const QVariant &year,
                      const QString &path, bool images);
    QString m_path;
    bool m_rescan = true;
};

} // namespace introvert::providers
