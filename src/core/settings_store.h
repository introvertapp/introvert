#pragma once

#include "sqlite.h"

#include <QMap>
#include <QString>
#include <QVariantMap>
#include <optional>
#include <stdexcept>

namespace introvert {

// Messages are deliberately independent of SQL errors, which may contain keys.
using SettingsError = DatabaseError;

struct AppSettings {
    QString timestampFormat = QStringLiteral("both");
    QString listingType = QStringLiteral("full");
    QString mediaProvider;
};

struct ProviderSettings {
    QString provider;
    QString remoteUrl;
    QString localRoot;
    bool fetchImages = false;
    double timeoutSeconds = 10;
    void validate() const;
};

enum class CredentialAction { Keep, Replace, Delete };

// Value-type service: only the path is retained. Every operation opens and closes
// its own Qt SQL connection on the calling thread, including worker operations.
class SettingsStore final {
public:
    explicit SettingsStore(QString databasePath);
    QString databasePath() const { return m_path; }
    void initialize() const;
    AppSettings load() const;
    AppSettings update(const QString &field, const QVariant &value) const;
    ProviderSettings loadProvider(const QString &provider) const;
    void saveProvider(const ProviderSettings &settings,
                      CredentialAction action = CredentialAction::Keep,
                      const QString &credential = {}) const;

    static QVariantMap endpointDefaults();
    static void validateEndpoints(const QVariantMap &values);
    QVariantMap loadEndpoints() const;
    void saveEndpoints(const QVariantMap &values, const QMap<QString, QString> &editedKeys = {}) const;

    static bool validWebhookUrl(const QString &value);
    QVariantMap loadWebhook() const;
    QVariantMap saveWebhook(const QVariantMap &values, const std::optional<QString> &editedKey = {}) const;

    // Services use independent connections. Media forms may expose saved values
    // only through their password-masked credential controls.
    std::optional<QString> credential(const QString &account) const;
    bool hasCredential(const QString &account) const;
    void setCredential(const QString &account, const std::optional<QString> &value) const;
    QVariantMap loadLocalMetadata(const QString &root) const;
    void saveLocalMetadata(const QString &root, const QVariantMap &values) const;

    QString detectionResources() const;
    void setDetectionResources(const QString &profile) const;
    bool visualRefinementEnabled() const;
    void setVisualRefinementEnabled(bool enabled) const;

    static QVariantMap inAppNotificationDefaults();
    QVariantMap loadInAppNotifications() const;
    void setInAppNotificationPreference(const QString &name, bool enabled) const;

    QVariantMap loadUpdates() const;
    void setAutomaticUpdates(bool automatic) const;
    void recordUpdateCheck(qint64 checkedAt) const;

private:
    QString m_path;
};

} // namespace introvert
