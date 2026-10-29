#pragma once

#include "catalog_segments.h"
#include <optional>
#include <stdexcept>

class QSqlDatabase;

namespace introvert::segments {

class SegmentError final : public std::invalid_argument {
public:
    explicit SegmentError(const char *message) : std::invalid_argument(message) {}
};

using SegmentChanges = QMap<QString, std::optional<QPair<double, double>>>;
using EpisodeTarget = QPair<providers::ShowSummary, providers::Episode>;

// Value service with per-operation SQL ownership. Compose with the same app.db
// as Settings/catalog; no separate segment database or schema is introduced.
class SegmentStore final {
public:
    explicit SegmentStore(QString databasePath) : m_path(std::move(databasePath)) {}
    void initialize() const;
    static void writeInTransaction(QSqlDatabase &db, const providers::ShowSummary &show, const providers::Episode &episode,
                                   const SegmentChanges &changes, const QString &source);
    SegmentValues load(const providers::ShowSummary &show, const providers::Episode &episode) const;
    void save(const providers::ShowSummary &show, const providers::Episode &episode,
              const SegmentChanges &changes, double duration, const QString &source = QStringLiteral("video_editor")) const;
    qsizetype deleteMany(const QList<EpisodeTarget> &episodes) const;
    bool isEdited(const providers::ShowSummary &show, const providers::Episode &episode) const;
    QVariantMap preferences() const;
    void setPreference(const QString &key, int value) const;
    std::optional<qint64> durationMs(const providers::Episode &episode) const;
private:
    QString m_path;
};

// Existing inline input syntax: seconds, MM:SS or HH:MM:SS, rounded to ms.
double parseTime(const QString &text);

} // namespace introvert::segments
