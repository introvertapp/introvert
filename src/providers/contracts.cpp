#include "contracts.h"
#include "json_values.h"
#include <QStringList>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>

namespace introvert::providers {

void normalizeChapters(MovieDetail &detail)
{
    const auto duration = detail.movie.durationMs.value_or(0);
    auto &chapters = detail.chapters;
    chapters.removeIf([duration](const Chapter &chapter) {
        return chapter.startMs < 0 || (duration > 0 && chapter.startMs >= duration);
    });
    std::stable_sort(chapters.begin(), chapters.end(), [](const Chapter &a, const Chapter &b) {
        return a.startMs < b.startMs;
    });
    chapters.erase(std::unique(chapters.begin(), chapters.end(), [](const Chapter &a, const Chapter &b) {
        return a.startMs == b.startMs;
    }), chapters.end());
    for (auto &chapter : chapters) {
        chapter.title = chapter.title.trimmed();
        if (chapter.endMs && (*chapter.endMs <= chapter.startMs || (duration > 0 && *chapter.endMs > duration)))
            chapter.endMs.reset();
    }
}

bool validMovieSegment(const ImportedSegment &segment, double durationSeconds)
{
    const auto [type, start, end] = segment;
    const auto lengthMs = std::round(end * 1000) - std::round(start * 1000);
    const auto maximumMs = type == SegmentType::Intro ? 300000 : type == SegmentType::Recap ? 1200000 : 1800000;
    return std::isfinite(start) && std::isfinite(end) && start >= 0 && end <= 21600
        && (type != SegmentType::Credits || std::round(start * 1000) >= 5000)
        && lengthMs >= 5000 && lengthMs <= maximumMs && (durationSeconds <= 0 || end <= durationSeconds);
}

bool movieSegmentsConflict(const ImportedSegment &left, const ImportedSegment &right)
{
    const auto gapMs = std::max(std::round(left.startSeconds * 1000), std::round(right.startSeconds * 1000))
        - std::min(std::round(left.endSeconds * 1000), std::round(right.endSeconds * 1000));
    return gapMs < (left.type == right.type ? 5000 : 0);
}

QList<ImportedSegment> movieChapterSegments(const MovieDetail &detail)
{
    static const QRegularExpression separators("[\\s_-]+");
    static const QStringList logoNames{"studiologo", "studiologos", "productionlogo", "productionlogos",
        "productioncompanylogo", "productioncompanylogos", "distributorlogo", "distributorlogos"};
    static const QStringList titleNames{"maintitle", "maintitles", "openingtitle", "openingtitles", "openingcredits",
        "titlesequence", "openingtitlesequence", "maintitle&credits", "maintitleandcredits"};
    const auto &chapters = detail.chapters;
    const auto duration = detail.movie.durationMs.value_or(0);
    QStringList names;
    names.reserve(chapters.size());
    for (const auto &chapter : chapters)
        names.append(chapter.title.toLower().remove(separators));

    QList<QList<ImportedSegment>> priorities(3);
    for (const auto type : {SegmentType::Intro, SegmentType::Credits, SegmentType::Recap, SegmentType::Preview}) {
        const auto kind = segmentName(type);
        for (qsizetype index = 0; index < chapters.size(); ++index) {
            const auto &name = names[index];
            const auto &chapter = chapters[index];
            const bool paired = name.endsWith("start");
            const auto label = paired ? name.chopped(5) : name;
            // Keep an explicit Intro ahead of aliases. A logo and a title
            // sequence can be separated by story footage.
            int priority = 0;
            if (label != kind) {
                if (type == SegmentType::Intro && logoNames.contains(label)) priority = 1;
                else if (type == SegmentType::Intro && titleNames.contains(label)) priority = 2;
                else if (type != SegmentType::Credits || (label != "endcredits" && label != "closingcredits" && label != "outro")) continue;
            }
            std::optional<qint64> end;
            if (paired) {
                const auto endName = label + "end";
                for (qsizetype next = index + 1; next < chapters.size(); ++next) {
                    if (names[next] == name) break;
                    if (names[next] == endName) { end = chapters[next].startMs; break; }
                }
            } else {
                end = chapter.endMs;
                if (!end) end = index + 1 < chapters.size() ? std::optional{chapters[index + 1].startMs} : detail.movie.durationMs;
            }
            if (!end) continue;
            const ImportedSegment segment{type, chapter.startMs / 1000., *end / 1000.};
            if (validMovieSegment(segment, duration / 1000.)) priorities[priority].append(segment);
        }
    }
    QList<ImportedSegment> segments;
    for (const auto &candidates : priorities) for (const auto &candidate : candidates)
        if (std::none_of(segments.cbegin(), segments.cend(), [&](const auto &other) {
                return movieSegmentsConflict(candidate, other);
            })) segments.append(candidate);
    return segments;
}

QString providerScope(const ProviderIdentity &identity, const QString &connection)
{
    const auto server = identity.serverId.value_or(QString());
    const auto value = QJsonDocument(QJsonArray{providerName(identity.provider),
        server.isEmpty() ? QString("connection") : QString("server"), server.isEmpty() ? connection : server}).toJson(QJsonDocument::Compact);
    return QString::fromLatin1(QCryptographicHash::hash(value, QCryptographicHash::Sha256).toHex());
}

QString itemWebUrl(const QString &serverUrl, const ProviderIdentity &identity)
{
    if (serverUrl.isEmpty() || identity.itemId.isEmpty() || identity.provider == ProviderId::Local) return {};
    const auto base = detail::baseUrl(serverUrl) + "/web/index.html#!/";
    if (identity.provider == ProviderId::Plex) {
        if (!identity.serverId || identity.serverId->isEmpty()) return {};
        return base + "server/" + detail::quote(*identity.serverId) + "/details?key="
            + detail::quote("/library/metadata/" + identity.itemId);
    }
    return base + (identity.provider == ProviderId::Emby ? "item" : "details")
        + "?id=" + detail::quote(identity.itemId)
        + (identity.serverId && !identity.serverId->isEmpty() ? "&serverId=" + detail::quote(*identity.serverId) : QString());
}

QString normalizedEpisodeOrder(QString value)
{
    value=value.toLower().remove(' ').remove('-').remove('_');
    const QMap<QString,QString> aliases{{"aired","official"},{"airedorder","official"},{"officialorder","official"},
        {"dvdorder","dvd"},{"altdvd","dvd"},{"absoluteorder","absolute"},{"abso","absolute"},
        {"alternateorder","alternate"},{"alttwo","alternate"},{"streaming","alternate"},{"streamingorder","alternate"},
        {"regionalorder","regional"},{"production","regional"},{"productionorder","regional"},{"defaultorder","default"}};
    value=aliases.value(value,value);
    return QStringList{"default","official","dvd","alternate","absolute","regional"}.contains(value)?value:QString();
}

QString providerName(ProviderId provider)
{
    switch (provider) {
    case ProviderId::Plex: return QStringLiteral("plex");
    case ProviderId::Emby: return QStringLiteral("emby");
    case ProviderId::Jellyfin: return QStringLiteral("jellyfin");
    case ProviderId::Local: return QStringLiteral("local");
    }
    throw ProviderError(QStringLiteral("Unsupported provider identifier"));
}

ProviderId providerId(const QString &name)
{
    for (const auto provider : {ProviderId::Plex, ProviderId::Emby, ProviderId::Jellyfin, ProviderId::Local})
        if (providerName(provider) == name) return provider;
    throw ProviderError(QStringLiteral("Unsupported provider identifier"));
}

QString segmentName(SegmentType kind)
{
    switch (kind) {
    case SegmentType::Recap: return QStringLiteral("recap");
    case SegmentType::Intro: return QStringLiteral("intro");
    case SegmentType::Credits: return QStringLiteral("credits");
    case SegmentType::Preview: return QStringLiteral("preview");
    }
    throw ProviderError(QStringLiteral("Unsupported imported segment type"));
}

SegmentType segmentType(const QString &name)
{
    for (const auto kind : {SegmentType::Recap, SegmentType::Intro, SegmentType::Credits, SegmentType::Preview})
        if (segmentName(kind) == name) return kind;
    throw ProviderError(QStringLiteral("Unsupported imported segment type"));
}

void checkCancelled(const Cancellation &cancelled)
{
    if (cancelled && cancelled->load(std::memory_order_relaxed)) throw Cancelled();
}

} // namespace introvert::providers
