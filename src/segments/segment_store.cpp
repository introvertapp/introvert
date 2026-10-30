#include "segment_store.h"
#include "identity.h"
#include "core/sqlite.h"
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <cmath>
#include <limits>

namespace introvert::segments {
namespace {
struct Identity { QStringList keys; qint64 season, episode; };

Identity identity(const providers::ShowSummary &show, const providers::Episode &episode)
{
    const auto keys = showKeys(show);
    if (keys.isEmpty() || !episode.seasonNumber || !episode.episodeNumber)
        throw SegmentError("Show identity and episode numbering are required to save segments");
    return {keys, *episode.seasonNumber, *episode.episodeNumber};
}

void write(QSqlDatabase &db, const Identity &id, const providers::Episode &episode, const SegmentChanges &changes, const QString &source)
{
    auto query = sql::execute(db, "SELECT COALESCE(MAX(revision),0)+1 FROM segments"); query.next();
    const auto revision = query.value(0); query.finish();
    for (auto change = changes.cbegin(); change != changes.cend(); ++change) for (const auto &key : id.keys)
        sql::execute(db, "INSERT INTO segments(show_key,season,episode,kind,start,end,deleted,source,provider,provider_episode,revision) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(show_key,season,episode,kind) DO UPDATE SET "
            "start=excluded.start,end=excluded.end,deleted=excluded.deleted,source=excluded.source,provider=excluded.provider,"
            "provider_episode=excluded.provider_episode,updated=CURRENT_TIMESTAMP,revision=excluded.revision",
            {key, id.season, id.episode, change.key(), change.value() ? QVariant(change.value()->first) : QVariant(),
             change.value() ? QVariant(change.value()->second) : QVariant(), !change.value(), sql::text(source),
             providers::providerName(episode.identity.provider), sql::text(episode.identity.itemId), revision});
}

QVariantMap defaults()
{
    return {{"frameStep", 0}, {"speed", 0}, {"autoPlay", false}, {"drawer", true}};
}
}

void SegmentStore::initialize() const
{
    if (!QDir().mkpath(QFileInfo(m_path).absolutePath())) throw DatabaseError("Cannot access Introvert app.db.");
    sql::Connection connection(m_path); sql::Transaction transaction(connection.db);
    initializeTables(connection.db); transaction.commit();
}
void SegmentStore::writeInTransaction(QSqlDatabase &db, const providers::ShowSummary &show, const providers::Episode &episode,
                                      const SegmentChanges &changes, const QString &source)
{
    write(db, identity(show, episode), episode, changes, source);
}

SegmentValues SegmentStore::load(const providers::ShowSummary &show, const providers::Episode &episode) const
{
    return loadCatalogSegments(m_path, {show, {{episode.seasonNumber, {episode}}}}).values.value(episode.identity.itemId);
}

void SegmentStore::save(const providers::ShowSummary &show, const providers::Episode &episode,
                        const SegmentChanges &changes, double duration, const QString &source) const
{
    const auto id = identity(show, episode);
    for (auto change = changes.cbegin(); change != changes.cend(); ++change) {
        if (change.key() != "recap" && change.key() != "intro" && change.key() != "credits" && change.key() != "preview")
            throw SegmentError("Unsupported segment type");
        if (change.value()) {
            const auto [start, end] = *change.value();
            if (!std::isfinite(start) || !std::isfinite(end) || start < 0 || !(5 <= end - start && end - start <= 180)
                || (duration > 0 && end > duration))
                throw SegmentError("Segment timestamps must be within the video and between 5 and 180 seconds long");
        }
    }
    std::optional<qint64> milliseconds;
    if (duration > 0) {
        const auto value = duration * 1000;
        if (!std::isfinite(value) || value >= double(std::numeric_limits<qint64>::max()))
            throw SegmentError("Episode duration is outside the supported range");
        // Stored durations use ties-to-even, unlike the inline timestamp parser.
        const auto floor = std::floor(value);
        milliseconds = qint64(floor + (value - floor > .5 || (value - floor == .5 && std::fmod(floor, 2) != 0) ? 1 : 0));
    }
    sql::Connection connection(m_path); sql::Transaction transaction(connection.db); initializeTables(connection.db);
    write(connection.db, id, episode, changes, source);
    if (milliseconds) sql::execute(connection.db, "INSERT OR REPLACE INTO episode_media VALUES(?,?,?)",
        {providers::providerName(episode.identity.provider), sql::text(episode.identity.itemId), *milliseconds});
    transaction.commit();
}

qsizetype SegmentStore::deleteMany(const QList<EpisodeTarget> &episodes, qint64 *statusRevision) const
{
    QList<Identity> plan;
    for (const auto &target : episodes) plan.append(identity(target.first, target.second));
    sql::Connection connection(m_path); sql::Transaction transaction(connection.db); initializeTables(connection.db);
    const SegmentChanges changes{{"recap", {}}, {"intro", {}}, {"credits", {}}, {"preview", {}}};
    for (qsizetype index = 0; index < episodes.size(); ++index) write(connection.db, plan[index], episodes[index].second, changes, "manual");
    const auto revision=currentRevision(connection.db);
    for(const auto &id:plan)for(const auto &key:id.keys)
        sql::execute(connection.db,"INSERT INTO episode_status_resets VALUES(?,?,?,?) ON CONFLICT(show_key,season,episode) DO UPDATE SET revision=excluded.revision",
            {key,id.season,id.episode,revision});
    transaction.commit();
    if(statusRevision)*statusRevision=revision;
    return plan.size();
}

bool SegmentStore::isEdited(const providers::ShowSummary &show, const providers::Episode &episode) const
{
    if (!episode.seasonNumber || !episode.episodeNumber) return false;
    const auto keys = showKeys(show);
    if (keys.isEmpty()) return false;
    QStringList placeholders; QVariantList arguments;
    for (const auto &key : keys) { placeholders.append("?"); arguments.append(key); }
    arguments.append(*episode.seasonNumber); arguments.append(*episode.episodeNumber);
    sql::Connection connection(m_path);
    auto query = sql::execute(connection.db, "SELECT kind,source,deleted FROM segments WHERE show_key IN ("
        + placeholders.join(',') + ") AND season=? AND episode=? ORDER BY revision DESC", arguments);
    QSet<QString> seen;
    while (query.next()) {
        const auto kind = query.value(0).toString();
        if (seen.contains(kind)) continue;
        seen.insert(kind);
        if (!query.value(2).toBool() && query.value(1).toString() == "video_editor") return true;
    }
    return false;
}

QVariantMap SegmentStore::preferences() const
{
    auto result = defaults();
    sql::Connection connection(m_path);
    auto query = sql::execute(connection.db, "SELECT name,value FROM preferences");
    while (query.next()) {
        const auto name = query.value(0).toString();
        if (result.contains(name)) result[name] = name == "autoPlay" || name == "drawer"
            ? QVariant(query.value(1).toBool()) : QVariant(query.value(1).toInt());
    }
    return result;
}

void SegmentStore::setPreference(const QString &key, int value) const
{
    if (!defaults().contains(key) || value < 0 || value > (key == "frameStep" || key == "speed" ? 2 : 1))
        throw SegmentError("Invalid editor preference");
    sql::Connection connection(m_path);
    sql::execute(connection.db, "INSERT INTO preferences VALUES(?,?) ON CONFLICT(name) DO UPDATE SET value=excluded.value", {key, value});
}

std::optional<qint64> SegmentStore::durationMs(const providers::Episode &episode) const
{
    if (episode.durationMs && *episode.durationMs) return episode.durationMs;
    sql::Connection connection(m_path);
    auto query = sql::execute(connection.db, "SELECT duration_ms FROM episode_media WHERE provider=? AND episode_id=?",
        {providers::providerName(episode.identity.provider), sql::text(episode.identity.itemId)});
    return query.next() ? std::optional<qint64>(query.value(0).toLongLong()) : std::nullopt;
}

double parseTime(const QString &text)
{
    // Accept the four information separators as whitespace alongside Unicode spaces.
    const auto whitespace = [](QChar value) { return value.isSpace() || (value.unicode() >= 0x1c && value.unicode() <= 0x1f); };
    qsizetype first = 0, last = text.size();
    while (first < last && whitespace(text[first])) ++first;
    while (last > first && whitespace(text[last - 1])) --last;
    QString normalized;
    for (const auto code : text.mid(first, last - first).toUcs4()) {
        if (QChar::category(code) == QChar::Number_DecimalDigit) normalized.append(QChar('0' + QChar::digitValue(code)));
        else {
            const char32_t character = code;
            normalized.append(QString::fromUcs4(&character, 1));
        }
    }
    const auto parts = normalized.split(':');
    const QRegularExpression syntax(QStringLiteral("\\A[0-9]+(?:\\.[0-9]+)?\\z"));
    if (parts.isEmpty() || parts.size() > 3) throw SegmentError("Invalid time format");
    double value = 0;
    for (qsizetype index = parts.size(); index > 0; --index) {
        const auto &part = parts[index - 1];
        if (!syntax.match(part).hasMatch()) throw SegmentError("Invalid time format");
        bool ok = false;
        const auto number = part.toDouble(&ok);
        if (!ok) throw SegmentError("Invalid time format");
        value += number * std::pow(60., parts.size() - index);
    }
    if (!std::isfinite(value) || !std::isfinite(value * 1000)) throw SegmentError("Invalid time format");
    return std::floor(value * 1000 + .5) / 1000;
}

} // namespace introvert::segments
