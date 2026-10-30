#pragma once

#include "settings_model.h"
#include "library/catalog_service.h"
#include "providers/metadata_service.h"
#include "segments/catalog_segments.h"
#include "segments/edit_locks.h"

namespace introvert::ui {

struct CatalogSelection {
    ProviderSettings saved;
    QString listing;
    bool operator==(const CatalogSelection &other) const;
    bool operator!=(const CatalogSelection &other) const { return !(*this == other); }
};

class LocalMetadataModel;
class SegmentManagementModel;

struct EpisodeContext {
    providers::ShowDetail detail;
    providers::Episode episode;
    quint64 revision;
};

class LibraryModel final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QObject *localMetadata READ localMetadata NOTIFY changed)
    Q_PROPERTY(QVariantList shows READ shows NOTIFY changed)
    Q_PROPERTY(QVariantList episodes READ episodes NOTIFY changed)
    Q_PROPERTY(QVariantMap show READ show NOTIFY changed)
    Q_PROPERTY(int season READ season NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool rebuildingCache READ rebuildingCache NOTIFY changed)
    Q_PROPERTY(double cacheRebuildProgress READ cacheRebuildProgress NOTIFY changed)
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString imageCacheSummary READ imageCacheSummary NOTIFY changed)
    Q_PROPERTY(QString episodeCount READ episodeCount NOTIFY changed)
public:
    LibraryModel(SettingsModel &settings, SettingsStore store, QString artworkRoot,
                 library::ProviderMap overrides = {}, providers::MetadataClient metadataClient = providers::MetadataClient(),
                 QObject *parent = nullptr);
    ~LibraryModel() override;
    QObject *localMetadata() const;
    QVariantList shows() const { return m_shows; }
    QVariantList episodes() const { return m_episodes; }
    QVariantList episodesForSeason(int number) const;
    QVariantMap show() const { return m_show; }
    int season() const { return m_season; }
    bool busy() const { return m_busy; }
    bool rebuildingCache() const { return m_rebuildGeneration && *m_rebuildGeneration == m_generation; }
    double cacheRebuildProgress() const { return m_rebuildProgress; }
    QString state() const { return m_state; }
    QString message() const { return m_message; }
    QString status() const;
    QString imageCacheSummary() const { return m_imageSummary; }
    QString episodeCount() const;
    Q_INVOKABLE void load();
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void rebuildCache();
    Q_INVOKABLE void clearImageCache();
    Q_INVOKABLE void clearLocalCache();
    Q_INVOKABLE void setBrowsing(bool active);
    Q_INVOKABLE void openShow(const QString &id);
    Q_INVOKABLE void selectSeason(int number);
    Q_INVOKABLE void closeShow();
    void close();
    std::optional<CatalogSelection> selection() const { return m_key; }
    std::optional<EpisodeContext> episodeContext(const QString &id) const;
    void updateSegments(quint64 revision, const QString &id, const segments::SegmentValues &values, bool edited);
signals:
    void changed();
    void errorOccurred(const QString &message);
    void cacheRebuildFailed(const QString &message);
private:
    friend class LocalMetadataModel;
    friend class SegmentManagementModel;
    friend class EditorModel;
    friend class SubmissionModel;
    friend class DetectionModel;
    segments::EditLocks m_editLocks;
    enum class Kind { Library, Show, Rebuild, Images, Stats };
    struct Result {
        library::CatalogResult catalog;
        std::optional<providers::ShowDetail> detail;
        QVariantMap display;
        segments::CatalogSegments segments;
        QString stats;
    };
    using Work = std::function<Result(const providers::Cancellation &, quint64)>;
    std::optional<CatalogSelection> settingsKey() const;
    void settingsChanged(bool configuration = false);
    void defer(const std::optional<CatalogSelection> &key);
    void refresh(bool force, bool clearLocal = false);
    void reloadLocalMetadata();
    quint64 begin();
    void submit(Kind kind, quint64 generation, Work work);
    void publish(quint64 generation, const library::CatalogResult &catalog);
    void progress(quint64 generation, double value);
    void finished(Kind kind, quint64 generation, const std::optional<Result> &result, const QString &error);

    SettingsModel &m_settings;
    SettingsStore m_store;
    library::MediaCache m_cache;
    library::CatalogService m_catalog;
    QThreadPool m_pool;
    LocalMetadataModel *m_metadata;
    providers::Cancellation m_cancel = std::make_shared<std::atomic_bool>(false);
    providers::Cancellation m_showCancel = std::make_shared<std::atomic_bool>(false);
    std::optional<CatalogSelection> m_key;
    std::optional<providers::ShowDetail> m_detail;
    segments::CatalogSegments m_segments;
    QVariantList m_shows, m_episodes;
    QVariantMap m_show;
    QString m_state = "ready", m_message = "Configure a media provider in Settings → Media to load your library.";
    QString m_imageSummary = "0.0 MB / 0 images";
    QString m_previousState, m_previousMessage;
    bool m_previousCached = false, m_previousPending = false;
    bool m_busy = false, m_closed = false, m_browsing = true, m_pending = false, m_cached = false;
    quint64 m_generation = 0;
    quint64 m_showGeneration = 0;
    bool m_showActive = false, m_catalogLoading = false;
    quint64 m_detailRevision = 0;
    std::optional<quint64> m_rebuildGeneration;
    double m_rebuildProgress = 0;
    int m_season = 1;
};

class LocalMetadataModel final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool fetching READ fetching NOTIFY changed)
    Q_PROPERTY(bool hasKey READ hasKey NOTIFY changed)
    Q_PROPERTY(QString credentialInput READ credentialInput NOTIFY changed)
    Q_PROPERTY(QVariantMap configuration READ configuration NOTIFY configurationChanged)
    Q_PROPERTY(QVariantMap credentials READ credentials NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(QVariantMap status READ status NOTIFY changed)
public:
    LocalMetadataModel(LibraryModel &library, providers::MetadataClient client);
    ~LocalMetadataModel() override;
    bool busy() const { return m_busy; }
    bool fetching() const { return m_busy && m_fetching; }
    bool hasKey() const { return m_hasKey; }
    QString credentialInput() const { return m_credential; }
    QVariantMap configuration() const { return m_configuration; }
    QVariantMap credentials() const { return m_credentials; }
    QString message() const { return m_message; }
    QVariantMap status() const { return m_status; }
    Q_INVOKABLE void activate();
    Q_INVOKABLE void queueKey(const QString &key);
    Q_INVOKABLE void saveKey(const QString &key);
    Q_INVOKABLE void setOption(const QString &name, const QString &value);
    Q_INVOKABLE void queueCredential(const QString &provider, const QString &key);
    Q_INVOKABLE void identify(const QString &showId, const QString &providerId);
    Q_INVOKABLE void fetch();
    Q_INVOKABLE void stop();
    void close();
signals:
    void changed();
    void errorOccurred(const QString &message);
    void keySaved();
    void configurationChanged();
private:
    enum class Kind { Status, Fetch };
    struct Result { bool hasKey = false; QVariantMap status; };
    using Work = std::function<Result(quint64)>;
    void start(Kind kind, Work work);
    void flushKey();
    LibraryModel &m_library;
    SettingsStore m_store;
    providers::MetadataService m_service;
    QThreadPool m_pool;
    QTimer m_timer;
    providers::Cancellation m_cancel = std::make_shared<std::atomic_bool>(false);
    std::optional<CatalogSelection> m_fetchKey;
    bool m_busy = false, m_fetching = false, m_hasKey = false, m_closed = false;
    quint64 m_generation = 0, m_keyRevision = 0;
    QString m_message, m_credential;
    QVariantMap m_status;
    QVariantMap m_configuration, m_credentials;
    QMap<QString,QString> m_pendingCredentials;
    QString m_root, m_targetShow;
};

} // namespace introvert::ui
