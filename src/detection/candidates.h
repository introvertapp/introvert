#pragma once

#include "audio_matching.h"
#include <QJsonObject>
#include <QStringList>
#include <memory>

namespace introvert::detection {

QString segmentName(SegmentType type);
QString recurrenceVersion(SegmentType type);
QString recurrenceEvidenceVersion(SegmentType type);
std::u32string unicodeOrder(const QString &value);
bool unicodeLess(const QString &a, const QString &b);
qint64 quantizedTimestamp(double seconds, int units = 1000000);
QByteArray canonicalIdentityJson(const QJsonValue &value);
QString identityDigest(const QJsonObject &value);

enum class RecurrenceRole { Maximal, Submatch };
using EpisodeRanges = std::vector<std::pair<QString, TimeRange>>;
using EvidenceValues = QList<QPair<QString, QVariant>>;
struct Evidence {
    QString schemaId, schemaVersion;
    EvidenceValues values;
    QVariant value(const QString &name, const QVariant &fallback = 0.) const;
    void validate() const;
};

class SegmentCandidate final {
public:
    SegmentCandidate(QString id, QString episode, SegmentType type, TimeRange range, QString generator, QString version,
        QStringList supporting, Evidence evidence, std::optional<QString> recurrenceId = {}, std::optional<RecurrenceRole> role = {},
        EpisodeRanges episodeRanges = {}, bool authoritative = false);
    const QString id, episode, generator, version;
    const SegmentType type;
    const TimeRange range;
    const QStringList supporting;
    const Evidence evidence;
    const std::optional<QString> recurrenceId;
    const std::optional<RecurrenceRole> role;
    const EpisodeRanges episodeRanges;
    const bool authoritative;
};
using Candidate = std::shared_ptr<const SegmentCandidate>;
using CandidateBatch = std::vector<Candidate>;

Candidate windowFallback(const QString &episode, SegmentType type, const TimeRange &window);
QString audioCandidateId(const QString &episode, SegmentType type, const TimeRange &range, const QStringList &supporting, int timestampUnits = 1000000);
bool candidateLess(const Candidate &a, const Candidate &b);
Candidate annotateCandidate(const Candidate &candidate, const QString &recurrenceId, RecurrenceRole role, const EpisodeRanges &ranges);

struct CandidateScore {
    QString candidateId;
    double score;
    int rank;
    QList<QPair<QString, double>> components;
    QStringList rejectionReasons;
};
struct ResolvedCandidate {
    QString resolverId, resolverVersion;
    QStringList rankedIds;
    std::vector<CandidateScore> scores;
    QString selectedId;
    bool forcedSelection;
};
ResolvedCandidate resolveCandidates(const QString &episode, SegmentType type, const TimeRange &window, const CandidateBatch &candidates);

} // namespace introvert::detection
