#pragma once

#include "contracts.h"
#include <QSet>

namespace introvert::providers {

class MetadataNotFound final : public ProviderError {
public:
    explicit MetadataNotFound(bool movie = false) : ProviderError(movie
        ? QStringLiteral("No movie matched the filename and year.")
        : QStringLiteral("No matching show was found. Use Identify to set a provider show ID."), 404) {}
};

// Metadata-provider authentication is isolated from media-server credentials.
// Image reads never inherit API headers; MetadataService schedules all work.
class MetadataClient final {
public:
    struct EpisodeData { QString title, tvdbId; };
    struct Artwork { QString poster, logo, error; };
    struct Record {
        QString id, title, poster, logo, source, artworkError;
        std::optional<qint64> year;
        ExternalIds ids;
        QMap<QPair<qint64,qint64>,EpisodeData> episodes;
    };
    explicit MetadataClient(QString tvmaze = QStringLiteral("https://api.tvmaze.com"),
                            QString tmdb = QStringLiteral("https://api.themoviedb.org/3"), double delaySeconds = .5,
                            QVariantMap additionalBases = {});
    Record identify(const QString &source, const ShowDetail &show, const QString &savedId,
                    const QString &order, const QVariantMap &credentials, const Cancellation &cancelled, bool includeArtwork = false) const;
    Artwork artwork(const QString &source, const Record &record, const ShowDetail &show,
                                  const QString &order, const QVariantMap &credentials, const Cancellation &cancelled) const;
    Record identifyMovie(const QString &source, const MovieSummary &movie, const QVariantMap &credentials,
                         const Cancellation &cancelled, bool includeArtwork) const;
    Artwork movieArtwork(const QString &source, const Record &record, const MovieSummary &movie,
                                       const QVariantMap &credentials, const Cancellation &cancelled) const;
    static QPair<QString,QString> sources(const QVariantMap &settings, bool movie);
    static QJsonValue read(const QString &url, const QVariantMap &parameters = {}, double timeoutSeconds = 10,
                           const Cancellation &cancelled = {});
    static QByteArray image(const QString &url, const Cancellation &cancelled = {});
    QJsonValue tv(const QString &path, const QVariantMap &parameters, const Cancellation &cancelled) const;
    static std::optional<qint64> positiveId(const QJsonValue &value);
private:
    struct Title {
        QString name;
        std::optional<qint64> year;
        ExternalIds ids;
        QList<qint64> seasons;
        bool movie = false;
    };
    Record identifyTitle(const QString &source, const Title &title, const QString &savedId,
                         const QString &order, const QVariantMap &credentials, const Cancellation &cancelled, bool includeArtwork) const;
    Artwork artworkFor(const QString &source, const Record &record, const Title &title,
                                    const QString &order, const QVariantMap &credentials, const Cancellation &cancelled) const;
    QString m_tvmaze, m_tmdb;
    double m_delay;
    QVariantMap m_bases;
    mutable QString m_tvdbToken, m_tvdbAuth;
    mutable std::optional<QSet<int>> m_tvdbMovieLogoTypes;
};

} // namespace introvert::providers
