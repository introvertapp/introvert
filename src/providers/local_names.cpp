#include "local_names.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>
#include <array>

namespace introvert::providers {
namespace {

QRegularExpression pattern(const QString &value, bool insensitive = true)
{
    return QRegularExpression(value, QRegularExpression::UseUnicodePropertiesOption
        | (insensitive ? QRegularExpression::CaseInsensitiveOption : QRegularExpression::NoPatternOption));
}

const auto release = pattern(QStringLiteral(R"re((?:^|[\s._-])(?:360p|480p|576p|720p|1080p|1080i|2160p|4320p|4k|8k|uhd|hdr10?|dv|dolby[ ._-]?vision|10[ ._-]?bit|xvid|x264|x265|h\.?264|h\.?265|hevc|av1|mpeg2|web[-_. ]?dl|web[-_. ]?rip|hdtv|pdtv|bluray|blu[-_. ]?ray|bdrip|brrip|dvdrip|hdrip|remux|proper|repack|rerip|internal|readnfo|amzn|atvp|dsnp|hmax|hulu|nflx|aac|flac|ac3|eac3|dts|truehd|ddp?5?\.?1|atmos)\b)re"));
const auto seasonPattern = pattern(QStringLiteral(R"re((?:^|[\s._-])(?:s|season|saison|series|stagione|temporada)[\s._-]*(\d{1,4})(?=$|[\s._-]))re"));
const auto seasonOnly = pattern(QStringLiteral(R"re(^(?:s|season|saison|series|stagione|temporada)[\s._-]*\d{1,4}(?:[\s._-]*(?:complete|disc|disk)\d*)?$)re"));

QString truncate(QString value)
{
    const auto match = release.match(value);
    if (match.hasMatch()) {
        value = value.left(match.capturedStart());
        while (!value.isEmpty() && QStringLiteral(" ._-").contains(value.back())) value.chop(1);
    }
    return value;
}

QString releaseTitle(QString value)
{
    value.replace(pattern(QStringLiteral(R"re((?:^|[\s._-])s\d{1,4}(?:\s*[-._]\s*s?\d{1,4})+(?=$|[\s._-]))re")), " ");
    value.replace(pattern(QStringLiteral(R"re((?:^|[\s._-])s\d{1,4}(?=$|[\s._-]))re")), " ");
    value.replace(pattern(QStringLiteral(R"re((?:^|[\s._-])(?:season|saison|series|stagione|temporada)s?[\s._-]*\d{1,4}(?:\s*[-._]\s*\d{1,4})?(?=$|[\s._-]))re")), " ");
    value = truncate(value);
    value.replace(pattern(QStringLiteral(R"re(\[[^\]]*\])re")), " ");
    value.replace(pattern(QStringLiteral(R"re(\([^)]*(?:720p|1080p|2160p|x264|x265|hevc|web|bluray|season|s\d{1,4})[^)]*\))re")), " ");
    value.replace(pattern(QStringLiteral(R"re(\b(complete[-_. ]?(?:series|season|seasons)?|extras?)\b)re")), " ");
    value.replace(pattern(QStringLiteral(R"re([._\-\[\]{}]+)re")), " ");
    return cleanTitle(value);
}

QString episodeTitle(QString value)
{
    value = truncate(value);
    value.replace(pattern(QStringLiteral(R"re(\b(360p|480p|576p|720p|1080p|1080i|2160p|4320p|4k|8k|x264|x265|h264|h265|hevc|av1|web[-_. ]?dl|webrip|bluray|bdrip|brrip|dvdrip|hdrip|proper|repack|aac|flac|dts|ddp?5?\.?1|atmos)\b)re")), " ");
    value.replace(pattern(QStringLiteral(R"re(\[[^\]]*\])re")), " ");
    value.replace(pattern(QStringLiteral(R"re(\([^)]*(?:720p|1080p|2160p|x264|x265|hevc|web|bluray)[^)]*\))re")), " ");
    value.replace(pattern(QStringLiteral(R"re([._-]+)re")), " ");
    return cleanTitle(value);
}

struct Token { qint64 season, episode; qsizetype start, end; };
struct TokenPattern { QRegularExpression expression; int season, episode, prefix; };

qint64 decimal(const QString &value)
{
    // Parse Unicode decimal digits matched by \d; QString's integer parser
    // accepts ASCII digits only.
    qint64 result = 0;
    for (const auto c : value.toUcs4()) result = result * 10 + QChar::digitValue(c);
    return result;
}

std::optional<Token> token(const QString &name, std::optional<qint64> inferred)
{
    static const std::array<TokenPattern, 6> patterns{{
        {pattern(QStringLiteral(R"re(s(\d{1,4})[\s._-]*e(?:p(?:isode)?)?[\s._-]*(\d{1,5})(?:[a-d])?(?:(?:e(?:p(?:isode)?)?[\s._-]*|[\s._-]+e(?:p(?:isode)?)?[\s._-]*|-(?:e)?)(?:\d{1,5})(?:[a-d])?)*)re")), 1, 2, 0},
        {pattern(QStringLiteral(R"re((^|[\s._\-\[({])(\d{1,2})[\s._-]*x[\s._-]*(\d{1,5})(?:(?:[\s._-]*x[\s._-]*|-)(?:\d{1,5}))*)re")), 2, 3, 1},
        {pattern(QStringLiteral(R"re((^|[\s._\-\[({])(?:season|saison|series|stagione|temporada)[\s._-]*(\d{1,4})[\s._-]+(?:episodes?|eps?|cap(?:itulo)?s?)[\s._-]*(\d{1,5}))re")), 2, 3, 1},
        {pattern(QStringLiteral(R"re(s(\d{2})\.(\d{2,3})(?=$|[\s._-]))re")), 1, 2, 0},
        {pattern(QStringLiteral(R"re((^|[\s._\-\[({])(\d{1,2})-(\d{2,3})(?=$|[\s._\-\])}]))re")), 2, 3, 1},
        {pattern(QStringLiteral(R"re((^|[\s._\-\[({])([1-9])((?:0[1-9]|[1-9]\d))(?=$|[\s._\-\])}]))re")), 2, 3, 1}
    }};
    for (const auto &spec : patterns) {
        const auto match = spec.expression.match(name);
        if (match.hasMatch()) return Token{decimal(match.captured(spec.season)), decimal(match.captured(spec.episode)),
            match.capturedStart() + (spec.prefix ? match.capturedLength(spec.prefix) : 0), match.capturedEnd()};
    }
    if (inferred) {
        auto match = pattern(QStringLiteral(R"re((^|[\s._\-\[({])(\d{2})(\d{2})(?=$|[\s._\-\])}]))re"), false).match(name);
        if (match.hasMatch() && decimal(match.captured(2)) == *inferred)
            return Token{*inferred, decimal(match.captured(3)), match.capturedStart() + match.capturedLength(1), match.capturedEnd()};
        match = pattern(QStringLiteral(R"re((^|[\s._\-\[({])(?:episodes?|eps?|e)[\s._-]*(\d{1,5})(?=$|[\s._\-\])}]))re")).match(name);
        if (match.hasMatch()) return Token{*inferred, decimal(match.captured(2)), match.capturedStart() + match.capturedLength(1), match.capturedEnd()};
        match = pattern(QStringLiteral(R"re(^(\d{1,3})(?=$|[\s._-]+))re"), false).match(name);
        if (match.hasMatch()) return Token{*inferred, decimal(match.captured(1)), match.capturedStart(), match.capturedEnd()};
    }
    return std::nullopt;
}
}

QString cleanTitle(QString value)
{
    value.replace(pattern(QStringLiteral(R"re([._]+)re")), " ");
    value.replace(pattern(QStringLiteral(R"re(\s+)re")), " ");
    value.replace(pattern(QStringLiteral(R"re(\s+\(\d{4}\)$)re")), "");
    value.replace(pattern(QStringLiteral(R"re(\s+\[\d{4}\]$)re")), "");
    return value.trimmed();
}

std::optional<ParsedEpisode> parseEpisode(const QString &filePath, const QString &rootPath)
{
    const auto relative = QDir(rootPath).relativeFilePath(filePath);
    if (relative.startsWith("../") || QDir::isAbsolutePath(relative)) return std::nullopt;
    const auto parts = relative.split('/');
    const auto directories = parts.mid(0, parts.size() - 1);
    const auto folder = directories.isEmpty() ? QString() : directories.first();
    std::optional<qint64> inferred;
    for (auto directory = directories.crbegin(); directory != directories.crend(); ++directory) {
        const auto match = seasonPattern.match(*directory);
        if (match.hasMatch()) { inferred = decimal(match.captured(1)); break; }
    }
    const auto stem = QFileInfo(filePath).completeBaseName();
    const auto found = token(stem, inferred);
    if (!found) return std::nullopt;
    const auto before = releaseTitle(stem.left(found->start));
    const auto only = seasonOnly.match(folder);
    const auto folderTitle = only.hasMatch() && only.capturedLength() == folder.size() ? QString() : releaseTitle(folder);
    const auto rootTitle = !directories.isEmpty() && folderTitle.isEmpty() ? releaseTitle(QFileInfo(rootPath).fileName()) : QString();
    const auto title = !folderTitle.isEmpty() ? folderTitle : !before.isEmpty() ? before : rootTitle;
    if (title.isEmpty()) return std::nullopt;
    const auto yearText = !folder.isEmpty() ? folder : !stem.left(found->start).isEmpty() ? stem.left(found->start) : rootTitle;
    const auto year = pattern(QStringLiteral(R"re((?:^|[\s._(\[{])(19\d{2}|20\d{2})(?:$|[\s._)\]}]))re")).match(yearText);
    const auto episode = episodeTitle(stem.mid(found->end));
    return ParsedEpisode{title, episode.isEmpty() ? std::nullopt : std::optional<QString>(episode), found->season, found->episode,
                         year.hasMatch() ? std::optional<qint64>(decimal(year.captured(1))) : std::nullopt};
}

} // namespace introvert::providers
