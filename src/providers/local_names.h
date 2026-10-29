#pragma once

#include <QString>
#include <optional>

namespace introvert::providers {

struct ParsedEpisode {
    QString showTitle;
    std::optional<QString> episodeTitle;
    qint64 seasonNumber;
    qint64 episodeNumber;
    std::optional<qint64> year;
};

QString cleanTitle(QString value);
std::optional<ParsedEpisode> parseEpisode(const QString &filePath, const QString &rootPath);

} // namespace introvert::providers
