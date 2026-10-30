#pragma once

#include <QAbstractListModel>
#include <QDateTime>
#include <QMutex>

namespace introvert::ui {
// Session diagnostics only. No credentials, media, or diagnostic database payloads
// are collected. The retained history and the cross-thread inbox are both bounded.
class LogModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int totalCount READ totalCount NOTIFY changed)
    Q_PROPERTY(int count READ count NOTIFY changed)
    Q_PROPERTY(QString search READ search WRITE setSearch NOTIFY changed)
    Q_PROPERTY(QString level READ level WRITE setLevel NOTIFY changed)
public:
    enum Role { TimestampRole = Qt::UserRole + 1, LevelRole, CategoryRole, MessageRole };
    explicit LogModel(QObject *parent = nullptr);
    ~LogModel() override;
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    int totalCount() const { return m_history.size(); }
    int count() const { return m_visible.size(); }
    QString search() const { return m_search; }
    QString level() const { return m_level; }
    void setSearch(const QString &value);
    void setLevel(const QString &value);
    void record(const QString &level, const QString &category, const QString &message);
    void startCapture();
    void stopCapture();
    static QString sanitized(QString text);
    Q_INVOKABLE void clear();
signals:
    void changed();
    void runtimeError();
private:
    struct Entry { QDateTime time; QString level, category, message; };
    static void handler(QtMsgType type, const QMessageLogContext &context, const QString &message);
    void drain();
    void refresh();
    bool matches(const Entry &entry) const;
    void append(Entry entry);
    QList<Entry> m_history, m_inbox;
    QList<int> m_visible;
    QString m_search, m_level = "all";
    bool m_scheduled = false;
};
}
