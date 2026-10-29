#pragma once

#include "library_model.h"
#include "segments/segment_store.h"

namespace introvert::ui {

class SegmentManagementModel final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList shows READ shows NOTIFY changed)
    Q_PROPERTY(QVariantList seasons READ seasons NOTIFY changed)
    Q_PROPERTY(QVariantList episodes READ episodes NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(QString episodeId READ episodeId NOTIFY changed)
    Q_PROPERTY(bool editingAllowed READ editingAllowed NOTIFY changed)
public:
    SegmentManagementModel(LibraryModel &library, segments::SegmentStore store,
                           library::ProviderMap overrides = {}, QObject *parent = nullptr);
    ~SegmentManagementModel() override;
    QVariantList shows() const { return m_library.shows(); }
    QVariantList seasons() const;
    QVariantList episodes() const;
    bool busy() const { return m_busy; }
    QString message() const { return m_message; }
    QString episodeId() const { return m_episode; }
    bool editingAllowed() const;
    Q_INVOKABLE bool canDelete(const QVariantList &targets) const;
    Q_INVOKABLE void browseShow(const QString &id);
    Q_INVOKABLE void saveInline(const QString &id, const QString &kind, const QString &start, const QString &end);
    Q_INVOKABLE void deleteInline(const QString &id, const QString &kind);
    Q_INVOKABLE void deleteBulk(const QVariantList &targets);
    void close();
signals:
    void changed();
    void deleted();
private:
    enum class Kind { Browse, Inline, Bulk };
    struct Result {
        std::optional<providers::ShowDetail> detail;
        segments::SegmentValues values;
        QString episode;
        bool edited = false;
        quint64 revision = 0;
        qsizetype count = 0;
        QList<QPair<QString, QString>> affected;
    };
    using Work = std::function<Result(const providers::Cancellation &)>;
    void libraryChanged();
    void start(Kind kind, Work work);
    void inlineChange(const QString &id, const QString &kind, const std::optional<QPair<QString, QString>> &times);
    void finished(Kind kind, quint64 generation, const std::optional<Result> &result, const QString &error);
    LibraryModel &m_library;
    segments::SegmentStore m_store;
    library::CatalogService m_catalog;
    QThreadPool m_pool;
    std::optional<CatalogSelection> m_key;
    QStringList m_catalogIds;
    std::optional<providers::ShowDetail> m_detail;
    providers::Cancellation m_cancel = std::make_shared<std::atomic_bool>(false);
    bool m_busy = false, m_closed = false;
    quint64 m_generation = 0;
    QString m_message, m_episode;
};

} // namespace introvert::ui
