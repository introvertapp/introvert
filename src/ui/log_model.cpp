#include "log_model.h"
#include <QRegularExpression>
#include <cstdio>
#include <utility>

namespace introvert::ui {
namespace {
constexpr int capacity = 1000;
QMutex captureMutex;
LogModel *capture = nullptr;
QtMessageHandler previousHandler = nullptr;
}
LogModel::LogModel(QObject *parent) : QAbstractListModel(parent) {}
LogModel::~LogModel() { stopCapture(); }
int LogModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : count(); }
QVariant LogModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= count()) return {};
    const auto &entry = m_history[m_visible[index.row()]];
    switch (role) {
    case TimestampRole: return entry.time.toString("HH:mm:ss");
    case LevelRole: return entry.level;
    case CategoryRole: return entry.category;
    case MessageRole: return entry.message;
    default: return {};
    }
}
QHash<int, QByteArray> LogModel::roleNames() const
{
    return {{TimestampRole,"timestamp"},{LevelRole,"level"},{CategoryRole,"category"},{MessageRole,"message"}};
}
QString LogModel::sanitized(QString text)
{
    // Qt and multimedia can include authenticated/signed URLs in diagnostics.
    // Hide the entire address, including path secrets and query parameters.
    text = text.left(16384);
    static const QRegularExpression url(R"(\b(?:https?|rtsp|file)://[^\s<>"']+)", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression authorization(R"(\b(?:Bearer|Basic)\s+[A-Za-z0-9_./+=-]+)", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression secret(R"((\b(?:api[-_ ]?key|api[-_]?token|x-plex-token|x-emby-token|x-introvert-webhook-key|access[-_]?token|token|password|authorization|credential|secret)\b["']?\s*[:=]\s*)(?:"[^"]*"|'[^']*'|[^\s,;]+))", QRegularExpression::CaseInsensitiveOption);
    text.replace(url, "[address hidden]");
    text.replace(authorization, "[authorization hidden]");
    text.replace(secret, "\\1[hidden]");
    text.replace(QChar::Null, QChar(' '));
    return text.left(4096).trimmed();
}
bool LogModel::matches(const Entry &entry) const
{
    return (m_level == "all" || entry.level == m_level)
        && (m_search.isEmpty() || (entry.message + ' ' + entry.category + ' ' + entry.level).contains(m_search, Qt::CaseInsensitive));
}
void LogModel::append(Entry entry)
{
    if (m_history.size() == capacity) {
        const bool visible = !m_visible.isEmpty() && m_visible.first() == 0;
        if (visible) beginRemoveRows({}, 0, 0);
        m_history.removeFirst();
        if (visible) m_visible.removeFirst();
        for (auto &index : m_visible) --index;
        if (visible) endRemoveRows();
    }
    const bool visible = matches(entry);
    if (visible) beginInsertRows({}, count(), count());
    m_history.append(std::move(entry));
    if (visible) { m_visible.append(m_history.size() - 1); endInsertRows(); }
}
void LogModel::refresh()
{
    beginResetModel(); m_visible.clear();
    for (int i = 0; i < m_history.size(); ++i) if (matches(m_history[i])) m_visible.append(i);
    endResetModel(); emit changed();
}
void LogModel::setSearch(const QString &value) { if (m_search == value) return; m_search = value; refresh(); }
void LogModel::setLevel(const QString &value)
{
    if (m_level == value || !QStringList{"all","info","warning","error","debug"}.contains(value)) return;
    m_level = value; refresh();
}
void LogModel::record(const QString &level, const QString &category, const QString &message)
{
    const auto clean = sanitized(message); if (clean.isEmpty()) return;
    append({QDateTime::currentDateTime(), level, sanitized(category), clean});
    emit changed();
}
void LogModel::clear()
{
    { QMutexLocker lock(&captureMutex); m_inbox.clear(); }
    beginResetModel(); m_history.clear(); m_visible.clear(); endResetModel(); emit changed();
}
void LogModel::startCapture()
{
    QMutexLocker lock(&captureMutex);
    if (capture) return;
    capture = this; previousHandler = qInstallMessageHandler(&LogModel::handler);
}
void LogModel::stopCapture()
{
    QMutexLocker lock(&captureMutex);
    if (capture != this) return;
    capture = nullptr; qInstallMessageHandler(previousHandler);
}
void LogModel::handler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    QtMessageHandler forward;
    {
        QMutexLocker lock(&captureMutex); forward = previousHandler;
        if (capture) {
            const auto level = type == QtDebugMsg ? "debug" : type == QtInfoMsg ? "info" : type == QtWarningMsg ? "warning" : "error";
            capture->m_inbox.append({QDateTime::currentDateTime(), level, sanitized(QString::fromUtf8(context.category)), sanitized(message)});
            while (capture->m_inbox.size() > capacity) capture->m_inbox.removeFirst();
            if (!capture->m_scheduled) {
                capture->m_scheduled = true;
                QMetaObject::invokeMethod(capture, &LogModel::drain, Qt::QueuedConnection);
            }
        }
    }
    if (forward) forward(type, context, message);
    else std::fprintf(stderr, "%s\n", qPrintable(qFormatLogMessage(type, context, message)));
}
void LogModel::drain()
{
    QList<Entry> entries;
    { QMutexLocker lock(&captureMutex); entries = std::exchange(m_inbox, {}); m_scheduled = false; }
    if (entries.isEmpty()) return;
    bool error = false;
    for (auto &entry : entries) { error |= entry.level == "error"; append(std::move(entry)); }
    emit changed(); if (error) emit runtimeError();
}
}
