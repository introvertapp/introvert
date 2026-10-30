#pragma once

#include "library_model.h"
#include "detection/service.h"

namespace introvert::ui {
class DetectionModel final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList queue READ queue NOTIFY changed)
    Q_PROPERTY(QVariantMap status READ status NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(bool active READ active NOTIFY changed)
public:
    DetectionModel(LibraryModel &library, SettingsStore settings, segments::SegmentStore segments,
        std::shared_ptr<detection::MediaDetectionRunner> runner = {}, detection::DetectionService::Completion completion = {}, QObject *parent = nullptr);
    ~DetectionModel() override;
    QVariantList queue() const { return m_rows; }
    QVariantMap status() const { return m_status; }
    QString message() const { return m_message.isEmpty() ? m_status.value("message").toString() : m_message; }
    bool active() const { const auto state = m_status.value("status").toString(); return state == "queued" || state == "running"; }
    Q_INVOKABLE void start(const QVariantList &kinds);
    Q_INVOKABLE void stop();
    void close();
signals:
    void changed();
    void errorOccurred(const QString &message);
    void jobFinished(const QString &title, int season, const QString &status, const QString &message);
    void diagnosticMessage(const QString &level, const QString &message);
    void jobStarted(const QString &title, int season);
private:
    struct Job { providers::Cancellation cancel; std::shared_ptr<segments::EditLocks::Lease> editLock; CatalogSelection selection; providers::ProviderIdentity show; };
    struct Pending { quint64 generation; CatalogSelection selection; providers::ProviderIdentity show; detection::DetectionUpdates updates; };
    bool sameConnection(const CatalogSelection &selection) const;
    bool sameShow(const providers::ProviderIdentity &show) const;
    void publish(const detection::DetectionUpdates &updates);
    void sync();
    void finished(const QString &id, const detection::DetectionUpdates &updates, const QString &error);
    LibraryModel &m_library;
    detection::DetectionJobStore m_store;
    detection::DetectionService m_service;
    QThreadPool m_pool;
    QMap<QString, Job> m_jobs;
    QList<Pending> m_pending;
    QVariantMap m_status;
    QVariantList m_rows;
    QString m_message;
    QString m_displayJob;
    std::optional<QString> m_context;
    bool m_closed = false;
};
} // namespace introvert::ui
