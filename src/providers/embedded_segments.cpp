#include "embedded_segments.h"
#include "json_values.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <algorithm>
#include <limits>

namespace introvert::providers {
namespace {
std::optional<double> seconds(const QJsonValue &value)
{
    if (value.isBool()) return {};
    const auto number = detail::finiteNumber(value);
    return number && *number >= 0 ? number : std::nullopt;
}

QString chapterName(const QJsonObject &chapter)
{
    const auto tags = chapter["tags"].toObject();
    for (auto it = tags.begin(); it != tags.end(); ++it)
        if (it.key().compare("title", Qt::CaseInsensitive) == 0)
            return it.value().toString().trimmed().toLower().remove(QRegularExpression("[\\s_-]+"));
    return {};
}
}

QList<ImportedSegment> embeddedSegments(const QJsonObject &probe)
{
    const auto chapters = probe["chapters"].toArray();
    const auto duration = seconds(probe["format"].toObject()["duration"]);
    QList<ImportedSegment> result;
    QSet<QString> found;
    for (qsizetype index = 0; index < chapters.size(); ++index) {
        const auto chapter = chapters[index].toObject();
        const auto name = chapterName(chapter);
        for (const auto &kind : {QString("recap"), QString("intro"), QString("credits"), QString("preview")}) {
            if (found.contains(kind) || (name != kind && name != kind + "start")) continue;
            const auto start = seconds(chapter["start_time"]);
            auto end = name == kind ? seconds(chapter["end_time"]) : std::optional<double>();
            if (!start) continue;
            // Explicit Start/End pairs use the End marker's start, rather than
            // the Start chapter's often zero-length interval.
            if (name == kind + "start") {
                for (qsizetype next = index + 1; next < chapters.size(); ++next) {
                    const auto marker = chapters[next].toObject();
                    if (chapterName(marker) == kind + "start") break;
                    if (chapterName(marker) == kind + "end") { end = seconds(marker["start_time"]); break; }
                }
            }
            if (!end || *end <= *start || (duration && *end > *duration)) continue;
            result.append({segmentType(kind), *start, *end}); found.insert(kind);
        }
    }
    return result;
}

bool inspectEmbeddedSegments(Episode &episode, const Cancellation &cancelled)
{
    checkCancelled(cancelled);
    if (!episode.mediaPath || !QFileInfo(*episode.mediaPath).isFile()) return false;
#ifdef Q_OS_WIN
    const auto name = QStringLiteral("ffprobe.exe");
#else
    const auto name = QStringLiteral("ffprobe");
#endif
    auto program = QDir(QCoreApplication::applicationDirPath()).filePath("bin/" + name);
    if (!QFileInfo(program).isExecutable()) program = QStandardPaths::findExecutable(name);
    if (program.isEmpty()) throw ProviderError("The ffprobe runtime is required to check embedded timestamps.");
    QProcess process;
    process.setProgram(program);
    process.setArguments({"-v", "error", "-protocol_whitelist", "file", "-show_chapters", "-show_entries",
        "format=duration:chapter=start_time,end_time:chapter_tags=title", "-of", "json", "-i", QFileInfo(*episode.mediaPath).absoluteFilePath()});
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start();
    QElapsedTimer timer; timer.start();
    QByteArray bytes;
    qint64 errorBytes = 0;
    bool exceeded = false;
    const auto drain = [&] {
        constexpr qint64 limit = 2 * 1024 * 1024;
        bytes += process.readAllStandardOutput();
        errorBytes += process.readAllStandardError().size();
        if (bytes.size() > limit || errorBytes > limit) exceeded = true;
    };
    while (process.state() != QProcess::NotRunning) {
        process.waitForFinished(25); drain();
        if ((cancelled && cancelled->load()) || timer.elapsed() >= 15000 || exceeded) {
            process.kill(); process.waitForFinished(-1);
            checkCancelled(cancelled);
            throw ProviderError("Embedded timestamp inspection timed out or exceeded its metadata limit.");
        }
    }
    drain(); checkCancelled(cancelled);
    if (process.error() == QProcess::FailedToStart) throw ProviderError("Cannot start the ffprobe runtime to check embedded timestamps.");
    // Unsupported/damaged media remains in the catalog and can still be
    // opened by the user. Never interpret partial probe output as timestamps.
    if (exceeded || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) return false;
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return false;
    const auto probe = document.object();
    episode.importedSegments = embeddedSegments(probe);
    const auto duration = seconds(probe["format"].toObject()["duration"]);
    if (duration && *duration * 1000 < double(std::numeric_limits<qint64>::max())) episode.durationMs = qint64(*duration * 1000);
    return true;
}

} // namespace introvert::providers
