#pragma once

#include "library_model.h"
#include "submissions/submission_plan.h"
#include "submissions/submission_store.h"
#include <condition_variable>
#include <deque>
#include <mutex>

namespace introvert::ui {

// Presentation only: one worker owns processing; SQLite owns durable job state.
class SubmissionModel final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList queue READ queue NOTIFY changed)
    Q_PROPERTY(QVariantList history READ history NOTIFY changed)
    Q_PROPERTY(QVariantMap statistics READ statistics NOTIFY changed)
    Q_PROPERTY(QVariantList endpoints READ endpoints NOTIFY changed)
    Q_PROPERTY(QVariantMap review READ review NOTIFY changed)
    Q_PROPERTY(QVariantMap episodeStatuses READ episodeStatuses NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool ready READ ready NOTIFY changed)
public:
    SubmissionModel(LibraryModel &library, SettingsStore settings, submissions::SubmissionStore store,
                    segments::SegmentStore segments, QObject *parent = nullptr);
    ~SubmissionModel() override;
    QVariantList queue() const { return m_queue; }
    QVariantList history() const { return m_history; }
    QVariantMap statistics() const { return m_statistics; }
    QVariantList endpoints() const { return m_endpoints; }
    QVariantMap review() const { return m_review; }
    QVariantMap episodeStatuses() const { return m_statuses; }
    QString message() const { return m_message; }
    bool busy() const { return m_busy; }
    bool ready() const { return m_plan.has_value() && !m_busy; }
    Q_INVOKABLE void prepare(const QVariantList &selection, const QString &endpoint);
    Q_INVOKABLE void confirm();
    Q_INVOKABLE void cancelReview();
    void start();
    void close();
signals:
    void changed();
    void prepared();
    void enqueued();
private:
    enum class Kind { Prepare, Enqueue, Snapshot, Error };
    struct Context {
        quint64 revision;
        std::optional<CatalogSelection> key;
        int season;
        QString identity;
        bool operator==(const Context &other) const;
        bool operator!=(const Context &other) const { return !(*this == other); }
    };
    struct Command {
        Kind kind;
        quint64 generation;
        std::optional<providers::ShowDetail> detail;
        QVariantList selection;
        QString endpoint;
        QVariantMap plan;
    };
    std::optional<Context> context() const;
    void libraryChanged();
    void push(Command command);
    void run();
    void finished(Kind kind, quint64 generation, const QVariant &value, const QString &error);
    LibraryModel &m_library;
    SettingsStore m_settings;
    submissions::SubmissionStore m_store;
    segments::SegmentStore m_segments;
    QThreadPool m_pool;
    providers::Cancellation m_stop = std::make_shared<std::atomic_bool>(false);
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<Command> m_commands;
    std::optional<Context> m_context;
    std::optional<QVariantMap> m_plan;
    QVariantList m_queue, m_history, m_endpoints;
    QVariantMap m_statistics, m_review, m_statuses;
    QString m_message;
    quint64 m_generation = 0;
    bool m_busy = false, m_started = false, m_closed = false;
};

} // namespace introvert::ui
