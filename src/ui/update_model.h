#pragma once

#include "core/settings_store.h"
#include "updates/release.h"
#include <QCryptographicHash>
#include <QFile>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <QThreadPool>
#include <atomic>
#include <functional>
#include <memory>

namespace introvert::providers { class AccessRetry; }
namespace introvert::ui {
class UpdateModel final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool automatic READ automatic NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool ready READ ready NOTIFY changed)
    Q_PROPERTY(bool enabled READ enabled CONSTANT)
    Q_PROPERTY(int progress READ progress NOTIFY changed)
    Q_PROPERTY(QString installedVersion READ installedVersion CONSTANT)
    Q_PROPERTY(QString availableVersion READ availableVersion NOTIFY changed)
    Q_PROPERTY(bool applicationUpdateAvailable READ applicationUpdateAvailable NOTIFY changed)
    Q_PROPERTY(QString latestVersion READ latestVersion NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(QString detectorInstalledVersion READ detectorInstalledVersion NOTIFY changed)
    Q_PROPERTY(QString detectorAvailableVersion READ detectorAvailableVersion NOTIFY changed)
    Q_PROPERTY(QString detectorLatestVersion READ detectorLatestVersion NOTIFY changed)
    Q_PROPERTY(QString updateKind READ updateKind NOTIFY changed)
public:
    explicit UpdateModel(std::optional<SettingsStore> store, QObject *parent = nullptr,
                         QNetworkAccessManager *network = nullptr,
                         QUrl feed = QUrl("https://api.github.com/repos/introvertapp/introvert/releases/latest"),
                         std::function<QString()> installationLocator = {}, QString packageFormat = {});
    ~UpdateModel() override;
    bool automatic() const { return m_automatic; }
    bool busy() const { return m_busy; }
    bool ready() const { return m_ready; }
    bool enabled() const { return m_store.has_value(); }
    int progress() const { return m_progress; }
    QString installedVersion() const;
    QString availableVersion() const { return m_offer.version; }
    bool applicationUpdateAvailable() const { return !m_appOffer.version.isEmpty() || (!m_detectorOffer && !m_offer.version.isEmpty()); }
    QString latestVersion() const { return m_latestVersion; }
    QString message() const { return m_message; }
    QString detectorInstalledVersion() const { return m_installedDetector; }
    QString detectorAvailableVersion() const { return m_detectorCandidate.version; }
    QString detectorLatestVersion() const { return m_detectorLatestVersion; }
    QString updateKind() const { return m_detectorOffer ? "detector" : "application"; }
    void start();
    void close();
    void setRestartGuard(std::function<bool()> guard) { m_restartGuard = std::move(guard); }
    void setRestartArguments(QStringList arguments) { m_restartArguments = std::move(arguments); }
    void reportFailedInstallation();
    void enableDetectorUpdates(QUrl feed = QUrl("https://api.github.com/repos/introvertapp/introvert/releases?per_page=100"));
    Q_INVOKABLE void setAutomatic(bool automatic);
    Q_INVOKABLE void check();
    Q_INVOKABLE void download();
    Q_INVOKABLE void downloadDetector();
    Q_INVOKABLE void install();
    Q_INVOKABLE void cancel();
signals:
    void changed();
    void errorOccurred(const QString &message);
    void offerChanged(const QString &kind, const QString &version);
    void diagnosticMessage(const QString &message);
    void restartRequested();
private:
    void automaticCheck();
    void checkDetector();
    void finishChecks(const QString &error = {});
    void checkFailed(const QString &message);
    void request(const QUrl &url, bool download, int redirects = 0);
    void fail(const QString &message);
    void prepare();
    void prepareStep(int step);
    QString installationPath() const;
    QString linuxFormat() const;
    std::optional<SettingsStore> m_store;
    QNetworkAccessManager m_network;
    QNetworkAccessManager *m_transport;
    QUrl m_feed, m_detectorFeed;
    updates::Offer m_appOffer, m_detectorCandidate;
    QString m_appCheckError, m_installedDetector, m_latestVersion, m_detectorLatestVersion;
    bool m_checkingDetector = false, m_detectorOffer = false;
    QPointer<QNetworkReply> m_reply;
    QProcess m_process;
    QTimer m_timer;
    QThreadPool m_installPool;
    std::shared_ptr<std::atomic_bool> m_installCancel;
    std::shared_ptr<QString> m_installResult;
    std::unique_ptr<QTemporaryDir> m_workspace;
    QFile m_download;
    QCryptographicHash m_hash{QCryptographicHash::Sha256};
    updates::Offer m_offer;
    QString m_message, m_payload, m_target;
    QByteArray m_metadata;
    std::function<bool()> m_restartGuard;
    std::function<QString()> m_installationLocator;
    QStringList m_restartArguments;
    QString m_packageFormat;
    quint64 m_generation = 0;
    QString m_accessOrigin;
    std::unique_ptr<providers::AccessRetry> m_access;
    bool m_automatic = false, m_busy = false, m_ready = false, m_closed = false, m_handedOff = false;
    int m_progress = 0;
    qint64 m_received = 0;
};
}
