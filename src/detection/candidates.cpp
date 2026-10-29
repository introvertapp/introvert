#include "candidates.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

namespace introvert::detection {
QString segmentName(SegmentType type) { return type == SegmentType::Intro ? "intro" : "credits"; }
QString recurrenceVersion(SegmentType type) { return type == SegmentType::Intro ? "audio-recurrence-candidates-v3" : "audio-recurrence-candidates-v7"; }
QString recurrenceEvidenceVersion(SegmentType type) { return type == SegmentType::Intro ? "1" : "3"; }
std::u32string unicodeOrder(const QString &value)
{
    std::u32string result; for (const auto c : value.toUcs4()) result.push_back(c); return result;
}
bool unicodeLess(const QString &a, const QString &b)
{
    const auto x = a.toUcs4(), y = b.toUcs4(); return std::lexicographical_compare(x.begin(), x.end(), y.begin(), y.end());
}
qint64 quantizedTimestamp(double seconds, int units)
{
    const double value = seconds * units, lower = std::floor(value), fraction = value - lower;
    if (!std::isfinite(value) || lower < double(std::numeric_limits<qint64>::min()) || lower >= double(std::numeric_limits<qint64>::max()))
        throw std::overflow_error("timestamp identity exceeds integer representation");
    const auto integer = qint64(lower); return integer + (fraction > .5 || (fraction == .5 && integer % 2 != 0));
}
QByteArray canonicalIdentityJson(const QJsonValue &value)
{
    if (value.isString()) {
        QByteArray result("\"");
        for (const auto c : value.toString()) {
            const auto n = c.unicode();
            switch (n) {
            case '"': result += "\\\""; break; case '\\': result += "\\\\"; break;
            case '\b': result += "\\b"; break; case '\f': result += "\\f"; break; case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break; case '\t': result += "\\t"; break;
            default: if (n < 32 || n >= 127) result += "\\u" + QByteArray::number(n, 16).rightJustified(4, '0'); else result += char(n);
            }
        }
        return result + '"';
    }
    if (value.isArray()) {
        QByteArray result("["); bool first = true;
        for (const auto &item : value.toArray()) { if (!first) result += ','; first = false; result += canonicalIdentityJson(item); }
        return result + ']';
    }
    if (value.isObject()) {
        QByteArray result("{"); const auto object = value.toObject(); auto keys = object.keys(); std::sort(keys.begin(), keys.end(), unicodeLess); bool first = true;
        for (const auto &key : keys) { if (!first) result += ','; first = false; result += canonicalIdentityJson(key) + ':' + canonicalIdentityJson(object[key]); }
        return result + '}';
    }
    if (value.isBool()) return value.toBool() ? "true" : "false";
    if (value.isNull()) return "null";
    if (value.isDouble() && value.toDouble() == double(value.toInteger())) return QByteArray::number(value.toInteger());
    throw std::invalid_argument("identity payload requires integer numbers");
}
QString identityDigest(const QJsonObject &value) { return QString::fromLatin1(QCryptographicHash::hash(canonicalIdentityJson(value), QCryptographicHash::Sha256).toHex()); }
QVariant Evidence::value(const QString &name, const QVariant &fallback) const
{
    for (const auto &item : values) if (item.first == name) return item.second; return fallback;
}
void Evidence::validate() const
{
    if (schemaId.isEmpty() || schemaVersion.isEmpty()) throw std::invalid_argument("evidence schema ID and version are required");
    QSet<QString> names;
    for (const auto &[name, value] : values) {
        if (names.contains(name)) throw std::invalid_argument("evidence names must be unique"); names.insert(name);
        switch (value.typeId()) {
        case QMetaType::Bool: case QMetaType::Int: case QMetaType::UInt: case QMetaType::LongLong: case QMetaType::ULongLong: case QMetaType::Double: case QMetaType::Float: break;
        default: throw std::invalid_argument("evidence values must be numeric or boolean");
        }
    }
}
SegmentCandidate::SegmentCandidate(QString id, QString episode, SegmentType type, TimeRange range, QString generator, QString version,
    QStringList supporting, Evidence evidence, std::optional<QString> recurrenceId, std::optional<RecurrenceRole> role, EpisodeRanges episodeRanges, bool authoritative)
    : id(std::move(id)), episode(std::move(episode)), generator(std::move(generator)), version(std::move(version)), type(type), range(range),
      supporting(std::move(supporting)), evidence(std::move(evidence)), recurrenceId(std::move(recurrenceId)), role(role), episodeRanges(std::move(episodeRanges)), authoritative(authoritative)
{
    if (this->id.isEmpty() || this->generator.isEmpty() || this->version.isEmpty()) throw std::invalid_argument("candidate and generator IDs and versions are required");
    this->evidence.validate();
    auto sorted = this->supporting; std::sort(sorted.begin(), sorted.end(), unicodeLess); sorted.removeDuplicates();
    if (sorted != this->supporting) throw std::invalid_argument("supporting episode IDs must be sorted and unique");
    if (!role) {
        if (this->recurrenceId || !this->episodeRanges.empty() || authoritative) throw std::invalid_argument("season recurrence metadata must be complete");
        return;
    }
    if (!this->recurrenceId || this->recurrenceId->isEmpty()) throw std::invalid_argument("season recurrence ID is required");
    const TimeRange *parent = nullptr;
    for (size_t i = 0; i < this->episodeRanges.size(); ++i) {
        const auto &[episodeId, r] = this->episodeRanges[i];
        if (i && !unicodeLess(this->episodeRanges[i - 1].first, episodeId)) throw std::invalid_argument("season recurrence episode ranges must be sorted and unique");
        if (episodeId == this->episode) parent = &r;
    }
    const bool same = parent && parent->start == range.start && parent->end == range.end;
    if (*role == RecurrenceRole::Maximal && !same) throw std::invalid_argument("maximal recurrence candidate must use its mapped range");
    if (*role == RecurrenceRole::Submatch && parent && (same || !parent->contains(range))) throw std::invalid_argument("recurrence submatch must be strictly contained by its parent");
    if (*role != RecurrenceRole::Maximal && *role != RecurrenceRole::Submatch) throw std::invalid_argument("unsupported season recurrence role");
    if (authoritative != (*role == RecurrenceRole::Maximal)) throw std::invalid_argument("only maximal season recurrence boundaries are authoritative");
}
Candidate windowFallback(const QString &episode, SegmentType type, const TimeRange &window)
{
    const QString generator = "window-fallback-candidate", version = "window-fallback-candidate-v1";
    const QJsonObject payload{{"episode_id", episode}, {"generator_id", generator}, {"generator_version", version}, {"segment_type", segmentName(type)},
        {"start_microseconds", quantizedTimestamp(window.start)}, {"end_microseconds", quantizedTimestamp(window.end)}};
    return std::make_shared<SegmentCandidate>("window-fallback:" + identityDigest(payload), episode, type, window, generator, version, QStringList{episode},
        Evidence{generator, "1", {{"window_fallback", true}, {"uses_media_content", false}, {"window_coverage_fraction", 1.}}});
}
QString audioCandidateId(const QString &episode, SegmentType type, const TimeRange &range, const QStringList &supporting, int timestampUnits)
{
    QJsonArray peers; for (const auto &id : supporting) peers.append(id);
    return "audio-recurrence:" + identityDigest({{"generator_id", "audio-recurrence-candidates"}, {"generator_version", recurrenceVersion(type)}, {"episode_id", episode},
        {"segment_type", segmentName(type)}, {"start", quantizedTimestamp(range.start, timestampUnits)}, {"end", quantizedTimestamp(range.end, timestampUnits)}, {"supporting_episode_ids", peers}});
}
bool candidateLess(const Candidate &a, const Candidate &b)
{
    const auto key = [](const Candidate &c) { return std::make_tuple(-c->evidence.value("peer_support_count").toLongLong(), -c->evidence.value("cluster_score").toDouble(),
        c->evidence.value("start_boundary_spread_seconds").toDouble() + c->evidence.value("end_boundary_spread_seconds").toDouble(), c->range.start, c->range.end, c->id); };
    return key(a) < key(b);
}
Candidate annotateCandidate(const Candidate &candidate, const QString &recurrenceId, RecurrenceRole role, const EpisodeRanges &ranges)
{
    return std::make_shared<SegmentCandidate>(candidate->id, candidate->episode, candidate->type, candidate->range, candidate->generator, candidate->version,
        candidate->supporting, candidate->evidence, recurrenceId, role, ranges, role == RecurrenceRole::Maximal);
}
} // namespace introvert::detection
