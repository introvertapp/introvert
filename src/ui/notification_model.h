#pragma once

#include "core/settings_store.h"
#include <QAbstractListModel>
#include <QVariantList>
#include <QSet>
#include <QTimer>

namespace introvert::ui {
class NotificationModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QVariantList items READ items NOTIFY changed)
    Q_PROPERTY(QVariantMap preferences READ preferences NOTIFY preferencesChanged)
public:
    explicit NotificationModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : m_items.size(); }
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override { return {{Qt::UserRole + 1, "notification"}}; }
    QVariantList items() const { return m_items; }
    QVariantMap preferences() const { return m_preferences; }
    void loadPreferences(SettingsStore store);
    bool allows(const QString &type) const;
    Q_INVOKABLE void setPreference(const QString &name, bool enabled);
    void post(const QString &title, const QString &message, const QString &key = {}, bool persistent = false,
        const QString &type = "errors", const QString &context = {}, const QString &guidance = {});
    void withdraw(const QString &key);
    Q_INVOKABLE void dismiss(const QString &id);
signals:
    void changed();
    void preferencesChanged();
    void errorOccurred(const QString &message);
private:
    void expire();
    std::optional<SettingsStore> m_store;
    QVariantMap m_preferences = SettingsStore::inAppNotificationDefaults();
    QVariantList m_items;
    QSet<QString> m_dismissed;
    QHash<QString, qint64> m_recent;
    QTimer m_timer;
    quint64 m_nextId = 0;
};
}
