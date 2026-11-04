#pragma once

#include "library_model.h"
#include "detection/service.h"
#include <QSet>
#include <QElapsedTimer>
#include <atomic>
#include <mutex>
#include <functional>

namespace introvert::ui {
class DetectionModel final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList queue READ queue NOTIFY changed)
    Q_PROPERTY(QVariantMap status READ status NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(bool active READ active NOTIFY changed)
    Q_PROPERTY(QVariantMap showDetection READ showDetection NOTIFY changed)
    Q_PROPERTY(QVariantMap seasonProgress READ seasonProgress NOTIFY changed)
    Q_PROPERTY(QVariantList activeSeasons READ activeSeasons NOTIFY changed)
    Q_PROPERTY(QStringList activeShowIds READ activeShowIds NOTIFY changed)
    Q_PROPERTY(QVariantMap libraryScan READ libraryScan NOTIFY changed)
    Q_PROPERTY(bool libraryScanActive READ libraryScanActive NOTIFY changed)
public:
    DetectionModel(LibraryModel &library, SettingsStore settings, segments::SegmentStore segments,
        std::shared_ptr<detection::MediaDetectionRunner> runner = {}, detection::DetectionService::Completion completion = {}, QObject *parent = nullptr, QString algorithmRoot = {});
    ~DetectionModel() override;
    QVariantList queue() const { return m_rows; }
    QVariantMap status() const { return m_status; }
    QString message() const { return m_message.isEmpty() ? m_status.value("message").toString() : m_message; }
    bool active() const { const auto state = m_status.value("status").toString(); return state == "queued" || state == "running"; }
    QVariantList activeSeasons() const;
    QVariantMap showDetection() const { return m_showDetection; }
    QVariantMap seasonProgress() const { return m_seasonProgress; }
    QStringList activeShowIds() const { return m_activeShowIds; }
    QVariantMap libraryScan() const { return m_scan; }
    bool libraryScanActive() const { return m_scan.value("active").toBool(); }
    Q_INVOKABLE void start(const QVariantList &kinds);
    Q_INVOKABLE void startSeasons(const QVariantList &seasons, const QVariantList &kinds);
    Q_INVOKABLE void stopSeasons(const QVariantList &seasons);
    Q_INVOKABLE void startLibraryScan(const QVariantList &kinds);
    Q_INVOKABLE void stopLibraryScan();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void refreshQueueOrder() { sync(); }
    void close();
signals:
    void changed();
    void errorOccurred(const QString &message);
    void jobFinished(const QString &title, int season, const QString &status, const QString &message);
    void diagnosticMessage(const QString &level, const QString &message);
    void jobStarted(const QString &title, int season);
    void detectorRequired(const QString &message);
private:
    struct Job { providers::Cancellation cancel; std::shared_ptr<segments::EditLocks::Lease> editLock; CatalogSelection selection; providers::ProviderIdentity show; int season; QStringList kinds; };
    struct Pending { quint64 generation; CatalogSelection selection; providers::ProviderIdentity show; detection::DetectionUpdates updates; };
    bool sameConnection(const CatalogSelection &selection) const;
    bool sameShow(const providers::ProviderIdentity &show) const;
    void publish(const detection::DetectionUpdates &updates);
    void sync();
    void scheduleSync();
    void dispatch(const QString &id, std::function<void()> task);
    void updateScanEstimate();
    void prepareShowDetection(const CatalogSelection &selection, const providers::ProviderIdentity &show);
    QString enqueue(const providers::ShowDetail &detail, int season, const QStringList &kinds, const CatalogSelection &selection);
    QStringList validatedKinds(const QVariantList &values);
    void withAlgorithm(std::function<void()> ready);
    void startSeasonsReady(const QVariantList &seasons, const QVariantList &values);
    void startLibraryScanReady(const QVariantList &values);
    void advanceScan();
    void scanJobFinished(const QString &id, const QString &status);
    void finishScan(const QString &status, const QString &message);
    void finished(const QString &id, const detection::DetectionUpdates &updates, const QString &error, const QVariantMap &completed);
    LibraryModel &m_library;
    detection::DetectionJobStore m_store;
    const bool m_requiresAlgorithm;
    const QString m_algorithmRoot;
    detection::DetectionService m_service;
    QThreadPool m_pool;
    std::mutex m_taskMutex;
    QList<QPair<QString, std::function<void()>>> m_tasks;
    bool m_dispatching = false;
    std::atomic_bool m_dispatchClosing{false};
    QThreadPool m_discoveryPool;
    QVariantMap m_scan, m_seasonProgress, m_showDetection;
    QMap<QString, QSet<QString>> m_showJobs;
    bool m_showDetectionObserved = false;
    QMap<int, QString> m_seasonJobs;
    QString m_showVisit;
    QElapsedTimer m_scanElapsed;
    QTimer m_scanClock;
    std::optional<CatalogSelection> m_scanSelection;
    providers::Cancellation m_scanCancel;
    QList<providers::ShowSummary> m_scanShows;
    QStringList m_scanKinds, m_activeShowIds;
    std::optional<providers::ShowDetail> m_scanDetail;
    QMap<QString, QPair<int, QStringList>> m_scanFollowups;
    QSet<QString> m_scanJobs, m_scanOwnedJobs, m_scanWaiting;
    int m_scanIndex = 0, m_scanSeasons = 0, m_scanSeasonsDone = 0;
    quint64 m_scanGeneration = 0;
    bool m_scanPreparing = false;
    bool m_checkingAlgorithm = false;
    QString m_verifiedAlgorithmLibrary;
    struct AlgorithmRequest { quint64 generation,showGeneration; std::function<void()> ready; };
    std::optional<AlgorithmRequest> m_algorithmRequest;
    QMap<QString, Job> m_jobs;
    QList<Pending> m_pending;
    QVariantMap m_status;
    QVariantList m_rows;
    QString m_message;
    QString m_displayJob;
    QString m_diagnosticsJob, m_diagnostics;
    std::optional<QString> m_context;
    bool m_closed = false;
    std::atomic_bool m_syncPending{false};
};
} // namespace introvert::ui
