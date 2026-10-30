#include "notification_model.h"
#include "log_model.h"
#include <QDateTime>

namespace introvert::ui {
NotificationModel::NotificationModel(QObject *parent) : QAbstractListModel(parent)
{
    m_timer.setInterval(200);
    connect(&m_timer, &QTimer::timeout, this, &NotificationModel::expire);
}
QVariant NotificationModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size() || role != Qt::UserRole + 1) return {};
    return m_items[index.row()];
}
void NotificationModel::loadPreferences(SettingsStore store)
{
    m_preferences = store.loadInAppNotifications(); m_store = std::move(store);
    emit preferencesChanged();
}
bool NotificationModel::allows(const QString &type) const
{
    return type != "enabled" && m_preferences.value("enabled").toBool() && m_preferences.value(type).toBool();
}
void NotificationModel::setPreference(const QString &name, bool enabled)
{
    if (!m_preferences.contains(name) || m_preferences.value(name).toBool() == enabled) return;
    try { if (m_store) m_store->setInAppNotificationPreference(name, enabled); }
    catch (...) {
        emit errorOccurred("Cannot save in-app notification preferences. Check application database access and try again.");
        emit preferencesChanged(); return;
    }
    m_preferences[name] = enabled;
    // Withdrawing a disabled type is not a dismissal. Valid update offers can
    // return when re-enabled; past temporary messages are never replayed.
    bool removed = false;
    for (int i = m_items.size() - 1; i >= 0; --i) if (!allows(m_items[i].toMap().value("type").toString())) {
        beginRemoveRows({}, i, i); m_items.removeAt(i); endRemoveRows(); removed = true;
    }
    if (removed) emit changed();
    emit preferencesChanged();
}
void NotificationModel::post(const QString &title, const QString &message, const QString &key, bool persistent,
    const QString &type, const QString &context, const QString &guidance)
{
    if (!allows(type)) return;
    const auto clean = LogModel::sanitized(message); if (clean.isEmpty()) return;
    const auto cleanContext = LogModel::sanitized(context), cleanGuidance = LogModel::sanitized(guidance);
    if (persistent && m_dismissed.contains(key)) return;
    const auto now = QDateTime::currentMSecsSinceEpoch();
    const auto identity = key.isEmpty() ? type + '\n' + title + '\n' + cleanContext + '\n' + clean : key;
    if (!persistent && m_recent.value(identity, 0) > now - 30000) return;
    for (const auto &value : m_items) if (value.toMap().value("key") == identity) return;
    if (!persistent) {
        for (auto it = m_recent.begin(); it != m_recent.end();) {
            if (it.value() <= now - 30000) it = m_recent.erase(it); else ++it;
        }
        if (m_recent.size() >= 128) m_recent.erase(m_recent.begin());
        m_recent[identity] = now;
        int transient = 0;
        for (const auto &value : m_items) if (!value.toMap().value("persistent").toBool()) ++transient;
        if (transient >= 5) for (int i = 0; i < m_items.size(); ++i) if (!m_items[i].toMap().value("persistent").toBool()) { beginRemoveRows({}, i, i); m_items.removeAt(i); endRemoveRows(); break; }
    }
    beginInsertRows({}, m_items.size(), m_items.size());
    m_items.append(QVariantMap{{"id",QString::number(++m_nextId)},{"key",identity},
        {"title",LogModel::sanitized(title)},{"message",clean},{"type",type},{"context",cleanContext},{"guidance",cleanGuidance},{"persistent",persistent},{"expires",persistent ? 0 : now + 5000}});
    endInsertRows();
    if (!persistent) m_timer.start();
    emit changed();
}
void NotificationModel::withdraw(const QString &key)
{
    bool removed = false;
    for (int i = m_items.size() - 1; i >= 0; --i) if (m_items[i].toMap().value("key") == key) { beginRemoveRows({}, i, i); m_items.removeAt(i); endRemoveRows(); removed = true; }
    if (removed) emit changed();
}
void NotificationModel::dismiss(const QString &id)
{
    for (int i = 0; i < m_items.size(); ++i) {
        const auto item = m_items[i].toMap(); if (item.value("id") != id) continue;
        if (item.value("persistent").toBool()) m_dismissed.insert(item.value("key").toString());
        beginRemoveRows({}, i, i); m_items.removeAt(i); endRemoveRows(); emit changed(); return;
    }
}
void NotificationModel::expire()
{
    const auto now = QDateTime::currentMSecsSinceEpoch(); bool removed = false, temporary = false;
    for (int i = m_items.size() - 1; i >= 0; --i) {
        const auto item = m_items[i].toMap(); if (item.value("persistent").toBool()) continue;
        if (item.value("expires").toLongLong() <= now) { beginRemoveRows({}, i, i); m_items.removeAt(i); endRemoveRows(); removed = true; } else temporary = true;
    }
    if (!temporary) m_timer.stop();
    if (removed) emit changed();
}
}
