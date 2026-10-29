#pragma once

#include "providers/contracts.h"
#include "core/sqlite.h"
#include <QHash>
#include <mutex>
#include <variant>

namespace introvert::library {

struct CacheLimits {
    qint64 imageBytes = 512 * 1024 * 1024;
    qint64 metadataBytes = 64 * 1024 * 1024;
    qint64 metadataEntries = 250;
};

// Worker-scheduled I/O with serialized shared state; SQL handles never cross
// threads. The cache owns no jobs or canonical/application records.
class MediaCache final {
public:
    static constexpr double seriesTtl = 86400, detailTtl = 86400, staleTtl = 86400, imageTtl = 30 * 86400;
    // Production uses the source limits. Small explicit limits support isolated
    // budget tests without allocating hundreds of MiB; they are not UI options.
    MediaCache(QString artworkRoot, QString databasePath, CacheLimits limits = {});
    QString scope(const providers::ProviderConfig &config, const QString &listing);
    std::optional<QList<providers::ShowSummary>> series(const QString &scope, const QString &item = {},
                                                        double ttl = seriesTtl, bool stale = false);
    std::optional<providers::ShowDetail> detail(const QString &scope, const QString &item,
                                               double ttl = detailTtl, bool stale = false);
    bool putSeries(const QString &scope, const QList<providers::ShowSummary> &shows, const QString &item = {});
    bool putDetail(const QString &scope, const providers::ShowDetail &detail, const QString &item);
    void invalidateDetails(const QString &scope);
    static QString imageKey(const QString &scope, const QString &path);
    std::optional<QString> image(const QString &scope, const QString &path, bool stale = false);
    std::optional<QString> putImage(const QString &scope, const QString &path, const QByteArray &bytes);
    void discardImage(const QString &scope, const QString &path);
    void clearImages(const std::optional<QString> &scope = {});
    QString stats();
private:
    std::unique_ptr<sql::Connection> connect();
    using CachedRecords = std::variant<QList<providers::ShowSummary>, providers::ShowDetail>;
    std::optional<CachedRecords> get(const QString &scope, const QString &kind, const QString &item, double ttl, bool stale);
    bool put(const QString &scope, const QString &kind, const QString &item, const QJsonValue &value);
    std::optional<QString> imageFile(const QString &name) const;
    QString m_root, m_path;
    CacheLimits m_limits;
    QHash<QByteArray, QString> m_scopes;
    QByteArray m_fallback;
    bool m_initialized = false;
    std::mutex m_mutex;
};

} // namespace introvert::library
