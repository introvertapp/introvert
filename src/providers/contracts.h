#pragma once

#include <QByteArray>
#include <QList>
#include <QJsonValue>
#include <QString>
#include <QVariantMap>
#include <atomic>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace introvert::providers {

enum class ProviderId { Plex, Emby, Jellyfin, Local };
enum class SegmentType { Recap, Intro, Credits, Preview };

QString providerName(ProviderId provider);
ProviderId providerId(const QString &name);
QString segmentName(SegmentType kind);
SegmentType segmentType(const QString &name);
QString normalizedEpisodeOrder(QString value);

class ProviderError : public std::runtime_error {
public:
    explicit ProviderError(const QString &message, std::optional<int> httpStatus = {})
        : std::runtime_error(message.toUtf8().constData()), m_httpStatus(httpStatus) {}
    std::optional<int> httpStatus() const { return m_httpStatus; }
private:
    std::optional<int> m_httpStatus;
};

class Cancelled final : public std::exception {
public:
    const char *what() const noexcept override { return "Media operation cancelled"; }
};

using Cancellation = std::shared_ptr<std::atomic_bool>;
void checkCancelled(const Cancellation &cancelled);

// Per-operation value snapshot. Never provide a debug/JSON serializer for this
// type: credentials and even endpoint URLs can contain authentication values.
struct ProviderConfig {
    explicit ProviderConfig(ProviderId id) : provider(id) {}
    ProviderId provider;
    std::optional<QString> remoteUrl;
    std::optional<QString> credential;
    std::optional<QString> localRoot;
    bool fetchImages = false;
    double timeoutSeconds = 10;
};

struct ProviderIdentity {
    ProviderId provider;
    QString itemId;
    std::optional<QString> serverId;
};

struct ExternalIds {
    std::optional<QString> imdb, tmdb, tvdb, zap2it, tvmaze;
};

struct ImportedSegment {
    SegmentType type;
    double startSeconds;
    double endSeconds;
};

struct LocalShowMetadata {
    std::optional<QString> localPath, metadataStatus, metadataError;
};

struct ShowSummary {
    ProviderIdentity identity;
    QString title;
    std::optional<qint64> year;
    std::optional<QString> posterUrl, logoUrl, itemUrl;
    ExternalIds externalIds;
    std::optional<qint64> seasonCount, episodeCount;
    std::optional<LocalShowMetadata> localMetadata;
};

struct Episode {
    ProviderIdentity identity;
    std::optional<QString> title;
    std::optional<qint64> seasonNumber, episodeNumber;
    std::optional<QString> itemUrl, mediaPath;
    QList<ImportedSegment> importedSegments;
    std::optional<qint64> durationMs;
    std::optional<QString> numberingSource, episodeOrder, tvdbEpisodeId;
};

struct Season {
    std::optional<qint64> number;
    QList<Episode> episodes;
};

struct ShowDetail {
    ShowSummary show;
    QList<Season> seasons;
    bool hasEpisodeAddressing = false;
};

// Synchronous domain boundary, scheduled on an owning worker by the library.
// Instances retain only short-lived discovery state and are used serially.
// Returned values have no canonical edit/segment persistence responsibilities.
class MediaProvider {
public:
    virtual ~MediaProvider() = default;
    virtual QList<ShowSummary> listSeries(const ProviderConfig &config, bool includeImages = true,
                                          const Cancellation &cancelled = {}) = 0;
    virtual std::optional<ShowDetail> getShow(const ProviderConfig &config, const QString &showId,
                                             bool includeImages = true, const Cancellation &cancelled = {}) = 0;
    virtual QByteArray artwork(const ProviderConfig &config, const QString &path,
                                const Cancellation &cancelled = {}) = 0;
    // Read-only authenticated metadata access is also used by the source's
    // stream resolver. Plex returns its unwrapped MediaContainer, as before.
    virtual QJsonValue request(const ProviderConfig &config, const QString &path,
                               const QVariantMap &parameters = {}, const Cancellation &cancelled = {}) = 0;
    virtual void invalidate() = 0;
};

} // namespace introvert::providers
