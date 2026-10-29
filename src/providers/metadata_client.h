#pragma once

#include "contracts.h"

namespace introvert::providers {

class MetadataNotFound final : public ProviderError {
public:
    MetadataNotFound() : ProviderError(QStringLiteral("TVmaze show match not found")) {}
};

// Headerless, absolute-URL metadata/artwork reads. No media-server credential
// can enter this client; MetadataService coordinates enrichment separately.
class MetadataClient final {
public:
    struct Match { std::optional<QString> id, error; };
    explicit MetadataClient(QString tvmaze = QStringLiteral("https://api.tvmaze.com"),
                            QString tmdb = QStringLiteral("https://api.themoviedb.org/3"), double delaySeconds = .5);
    static QJsonValue read(const QString &url, const QVariantMap &parameters = {}, double timeoutSeconds = 10,
                           const Cancellation &cancelled = {});
    static QByteArray image(const QString &url, const Cancellation &cancelled = {});
    QJsonValue tv(const QString &path, const QVariantMap &parameters, const Cancellation &cancelled) const;
    Match tmdbId(const std::optional<QString> &imdb, const std::optional<qint64> &tvdb, const std::optional<QString> &key,
                 double timeoutSeconds, const Cancellation &cancelled) const;
    static std::optional<qint64> positiveId(const QJsonValue &value);
private:
    QString m_tvmaze, m_tmdb;
    double m_delay;
};

} // namespace introvert::providers
