#pragma once

#include "core/settings_store.h"
#include "notifications/webhook_service.h"

#include <QObject>
#include <QThreadPool>
#include <QTimer>

namespace introvert::ui {

class ProviderSettingsModel final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString provider READ provider NOTIFY changed)
    Q_PROPERTY(QString address READ address NOTIFY changed)
    Q_PROPERTY(bool fetchImages READ fetchImages NOTIFY changed)
    Q_PROPERTY(bool dirty READ dirty NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(QString credentialInput READ credentialInput NOTIFY changed)
    Q_PROPERTY(bool credentialEdited READ credentialEdited NOTIFY changed)
    Q_PROPERTY(QString credentialStatus READ credentialStatus NOTIFY changed)
public:
    ProviderSettingsModel(SettingsStore store, const QString &provider, QObject *parent = nullptr);
    ~ProviderSettingsModel() override;
    QString provider() const { return m_provider; }
    QString address() const { return m_address; }
    bool fetchImages() const { return m_images; }
    bool dirty() const;
    QString message() const { return m_message; }
    QString credentialInput() const { return m_credential; }
    bool credentialEdited() const { return m_edited; }
    QString credentialStatus() const { return m_status; }
    void select(const QString &provider);
    Q_INVOKABLE void setAddress(const QString &value);
    Q_INVOKABLE void setFetchImages(bool value);
    Q_INVOKABLE void setCredentialInput(const QString &value);
    Q_INVOKABLE void setDirectoryUrl(const QString &value);
    Q_INVOKABLE void flush();
    Q_INVOKABLE bool saveCredentialDraft(bool edited, const QString &credential);
signals:
    void changed();
    void errorOccurred(const QString &message);
    void saved();
    void cleared();
private:
    struct Draft { QString address; bool images; QString credential; bool edited; };
    bool save(CredentialAction action, const QString &credential);
    void refreshStatus();
    void edited();
    SettingsStore m_store;
    QTimer m_timer;
    QString m_provider, m_address, m_credential, m_message, m_status, m_savedAddress;
    bool m_images = false, m_savedImages = false, m_edited = false;
    double m_timeout = 10;
    QMap<QString, Draft> m_drafts;
};

class EndpointSettingsModel final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap values READ values NOTIFY changed)
    Q_PROPERTY(QVariantMap keyStatus READ keyStatus NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
public:
    explicit EndpointSettingsModel(SettingsStore store, QObject *parent = nullptr);
    ~EndpointSettingsModel() override;
    QVariantMap values() const { return m_values; }
    QVariantMap keyStatus() const { return m_keyStatus; }
    bool busy() const { return m_busy; }
    QString message() const { return m_message; }
    Q_INVOKABLE void setValue(const QString &name, const QVariant &value);
    Q_INVOKABLE void editKey(const QString &name, const QString &value);
    Q_INVOKABLE void activate();
    Q_INVOKABLE void save();
    void close();
signals:
    void changed();
    void errorOccurred(const QString &message);
    void saved();
private:
    void edited();
    SettingsStore m_store;
    QVariantMap m_values, m_keyStatus;
    QMap<QString, QString> m_keys;
    QTimer m_timer;
    QThreadPool m_pool;
    QString m_message;
    bool m_busy = false, m_closed = false;
    quint64 m_revision = 0, m_persistedRevision = 0;
};

class WebhookSettingsModel final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap values READ values NOTIFY changed)
    Q_PROPERTY(bool hasKey READ hasKey NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool canTest READ canTest NOTIFY changed)
    Q_PROPERTY(bool canSave READ canSave NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
public:
    explicit WebhookSettingsModel(SettingsStore store, QObject *parent = nullptr);
    ~WebhookSettingsModel() override;
    QVariantMap values() const { return m_values; }
    bool hasKey() const { return m_hasKey; }
    bool busy() const { return m_busy; }
    bool canTest() const;
    bool canSave() const;
    QString message() const;
    Q_INVOKABLE void setValue(const QString &name, const QVariant &value);
    Q_INVOKABLE void editKey(const QString &value);
    Q_INVOKABLE void activate();
    Q_INVOKABLE void save();
    Q_INVOKABLE void test();
    void close();
signals:
    void changed();
    void errorOccurred(const QString &message);
    void saved();
private:
    void edited();
    SettingsStore m_store;
    QVariantMap m_values;
    std::optional<QString> m_key;
    QTimer m_timer;
    QThreadPool m_pool;
    QString m_message;
    bool m_busy = false, m_closed = false, m_hasKey = false;
    providers::Cancellation m_testCancel = std::make_shared<std::atomic_bool>(false);
    quint64 m_revision = 0, m_persistedRevision = 0;
};

class SettingsModel final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString timestampFormat READ timestampFormat NOTIFY changed)
    Q_PROPERTY(QString listingType READ listingType NOTIFY changed)
    Q_PROPERTY(QString detectionResources READ detectionResources NOTIFY changed)
    Q_PROPERTY(QString detectionEffort READ detectionEffort NOTIFY changed)
    Q_PROPERTY(bool visualRefinementEnabled READ visualRefinementEnabled NOTIFY changed)
    Q_PROPERTY(QString mediaProvider READ mediaProvider NOTIFY changed)
    Q_PROPERTY(QObject *mediaSettings READ mediaSettings CONSTANT)
    Q_PROPERTY(QObject *endpointSettings READ endpointSettings CONSTANT)
    Q_PROPERTY(QObject *webhookSettings READ webhookSettings CONSTANT)
public:
    explicit SettingsModel(SettingsStore store, QObject *parent = nullptr);
    ~SettingsModel() override;
    QString timestampFormat() const { return m_snapshot.timestampFormat; }
    QString listingType() const { return m_snapshot.listingType; }
    QString detectionResources() const { return m_detectionResources; }
    QString detectionEffort() const { return m_detectionResources=="low"?"light":m_detectionResources=="full"?"high":"medium"; }
    bool visualRefinementEnabled() const { return m_visualRefinementEnabled; }
    QString mediaProvider() const { return m_snapshot.mediaProvider; }
    QObject *mediaSettings() const { return m_media; }
    QObject *endpointSettings() const { return m_endpoints; }
    QObject *webhookSettings() const { return m_webhooks; }
    Q_INVOKABLE void setTimestampFormat(const QString &value);
    Q_INVOKABLE void setListingType(const QString &value);
    Q_INVOKABLE void setDetectionResources(const QString &value);
    Q_INVOKABLE void setDetectionEffort(const QString &value);
    Q_INVOKABLE void setVisualRefinementEnabled(bool value);
    Q_INVOKABLE void setMediaProvider(const QString &value);
    void close();
signals:
    void changed();
    void error(const QString &message);
private:
    void update(const QString &field, const QVariant &value);
    SettingsStore m_store;
    AppSettings m_snapshot;
    QString m_detectionResources;
    bool m_visualRefinementEnabled = true;
    ProviderSettingsModel *m_media;
    EndpointSettingsModel *m_endpoints;
    WebhookSettingsModel *m_webhooks;
};

} // namespace introvert::ui
