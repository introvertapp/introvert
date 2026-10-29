#include "media_probe.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <cerrno>
#include <limits>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace introvert::detection {
namespace {
bool truth(const QVariant &value)
{
    if (value.isNull()) return false;
    if (value.metaType().id() == QMetaType::QString) return !value.toString().isEmpty();
    if (value.metaType().id() == QMetaType::QVariantList) return !value.toList().isEmpty();
    if (value.metaType().id() == QMetaType::QVariantMap) return !value.toMap().isEmpty();
    return value.toDouble() != 0;
}
QString digits(QString text)
{
    text = text.trimmed();
    for (auto &c : text) if (c.digitValue() >= 0 && c.category() == QChar::Number_DecimalDigit) c = QChar('0' + c.digitValue());
    for (qsizetype i = 0; i < text.size(); ++i) if (text[i] == '_'
        && (i == 0 || i + 1 == text.size() || !text[i - 1].isDigit() || !text[i + 1].isDigit())) return {};
    return text.remove('_');
}
std::optional<double> duration(const QVariant &value)
{
    if (value.isNull()) return {};
    bool ok = true;
    const auto number = value.metaType().id() == QMetaType::Bool ? double(value.toBool())
        : value.metaType().id() == QMetaType::QString ? digits(value.toString()).toDouble(&ok) : value.toDouble(&ok);
    return ok && std::isfinite(number) && number > 0 ? std::optional<double>(number) : std::nullopt;
}
std::optional<double> clockDuration(const QJsonValue &tag)
{
    static const QRegularExpression clock("\\A([0-9]{2,}):([0-5][0-9]):([0-5][0-9](?:\\.[0-9]{1,9})?)\\z");
    const auto match=clock.match(tag.toString());
    bool hoursOk=false; const auto hours=match.captured(1).toDouble(&hoursOk);
    if(!match.hasMatch() || !hoursOk)return {};
    return duration(hours*3600.+match.captured(2).toDouble()*60.+match.captured(3).toDouble());
}
std::optional<double> taggedAudioDuration(const QJsonObject &tags)
{
    auto endpoint=clockDuration(tags.value("DURATION"));
    if(endpoint)return endpoint;
    // Matroska can qualify this stream tag with an ISO 639 language.
    // All qualified declarations must parse and agree.
    static const QRegularExpression qualified("\\ADURATION-[a-zA-Z]{3}\\z");
    bool conflict=false;
    for(auto tag=tags.begin();tag!=tags.end();++tag) {
        if(!qualified.match(tag.key()).hasMatch())continue;
        const auto candidate=clockDuration(tag.value());
        if(!candidate || (endpoint && std::abs(*candidate-*endpoint)>1e-9))conflict=true;
        else endpoint=candidate;
    }
    return conflict?std::nullopt:endpoint;
}
std::optional<qint64> index(const QVariant &value)
{
    if (value.isNull()) return {};
    bool ok = false;
    if (value.metaType().id() == QMetaType::QString) {
        const auto result = digits(value.toString()).toLongLong(&ok); return ok ? std::optional<qint64>(result) : std::nullopt;
    }
    if (value.metaType().id() == QMetaType::Double) {
        const auto number = value.toDouble();
        if (!std::isfinite(number) || number < -9223372036854775808. || number >= 9223372036854775808.) return {};
        return qint64(number);
    }
    const auto number = value.toLongLong(&ok); return ok ? std::optional<qint64>(number) : std::nullopt;
}
const MediaStream *select(const QList<MediaStream> &streams, bool excludePictures)
{
    const MediaStream *first = nullptr;
    for (const auto &stream : streams) {
        if (excludePictures && stream.isAttachedPicture) continue;
        if (!first) first = &stream;
        if (stream.isDefault) return &stream;
    }
    return first;
}
}

QString normalizedMediaPath(const QString &path)
{
    auto expanded = path;
    if (path == "~" || path.startsWith("~/")) expanded = QDir::homePath() + path.mid(1);
#ifndef Q_OS_WIN
    else if (path.startsWith('~')) {
        const auto slash = path.indexOf('/'); const auto user = path.mid(1, slash < 0 ? -1 : slash - 1).toUtf8();
        const auto size = sysconf(_SC_GETPW_R_SIZE_MAX); QByteArray buffer(size > 0 ? size : 16384, '\0');
        struct passwd entry; struct passwd *found = nullptr;
        if (getpwnam_r(user.constData(), &entry, buffer.data(), size_t(buffer.size()), &found) == 0 && found)
            expanded = QString::fromLocal8Bit(found->pw_dir) + (slash < 0 ? QString() : path.mid(slash));
    }
#endif
    // abspath/normpath does not resolve symlinks; metadata below follows them.
    return QDir::cleanPath(QFileInfo(expanded).absoluteFilePath());
}

MediaIdentity readMediaIdentity(const QString &path)
{
    const auto normalized = normalizedMediaPath(path);
    qint64 bytes, modified;
#ifdef Q_OS_WIN
    // A normal handle follows symlinks, matching os.stat. See Microsoft's
    // BY_HANDLE_FILE_INFORMATION / symbolic-link-effects documentation.
    HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(normalized).utf16()), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        throw DetectionError(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? "filenotfounderror"
            : error == ERROR_ACCESS_DENIED ? "permissionerror" : "oserror", "Cannot inspect media file identity.");
    }
    BY_HANDLE_FILE_INFORMATION info; const bool ok = GetFileInformationByHandle(handle, &info); CloseHandle(handle);
    if (!ok) throw DetectionError("oserror", "Cannot inspect media file identity.");
    const auto size = (quint64(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    const auto ticks = (quint64(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime;
    constexpr quint64 epoch = 116444736000000000;
    const auto delta = ticks >= epoch ? ticks - epoch : epoch - ticks;
    if (size > quint64(std::numeric_limits<qint64>::max()) || delta > quint64(std::numeric_limits<qint64>::max() / 100))
        throw DetectionError("overflowerror", "Media identity exceeds the supported integer range.");
    bytes = qint64(size); modified = qint64(delta) * (ticks >= epoch ? 100 : -100);
#else
    struct stat info;
    if (::stat(QFile::encodeName(normalized).constData(), &info) != 0)
        throw DetectionError(errno == ENOENT ? "filenotfounderror" : errno == ENOTDIR ? "notadirectoryerror"
            : errno == EACCES || errno == EPERM ? "permissionerror" : "oserror", "Cannot inspect media file identity.");
#ifdef Q_OS_MACOS
    const auto time = info.st_mtimespec;
#else
    const auto time = info.st_mtim;
#endif
    if (time.tv_sec < std::numeric_limits<qint64>::min() / 1000000000 || time.tv_sec > std::numeric_limits<qint64>::max() / 1000000000)
        throw DetectionError("overflowerror", "Media identity exceeds the supported integer range.");
    const auto seconds = qint64(time.tv_sec) * 1000000000;
    if (seconds > std::numeric_limits<qint64>::max() - time.tv_nsec)
        throw DetectionError("overflowerror", "Media identity exceeds the supported integer range.");
    bytes = info.st_size; modified = seconds + time.tv_nsec;
#endif
    return {normalized, bytes, modified};
}

MediaInfo parseMediaInfo(const QJsonObject &payload, const MediaIdentity &identity)
{
    QList<MediaStream> audio, video;
    QMap<qint64,double> audioEnds;
    for (const auto &value : payload.value("streams").toArray()) {
        const auto row = value.toObject(); const auto kind = row.value("codec_type").toString();
        if (kind != "audio" && kind != "video") continue;
        const auto number = index(row.value("index").toVariant()); if (!number) continue;
        const auto flags = row.value("disposition").toObject();
        MediaStream stream{*number, kind, duration(row.value("duration").toVariant()), truth(flags.value("default").toVariant()), truth(flags.value("attached_pic").toVariant())};
        // Non-zero/unknown origins need explicit timeline mapping. Do not guess
        // it, change the legacy duration fields, or trust a decoded short read.
        bool startOk=false;
        const auto start=row.value("start_time").toVariant().toDouble(&startOk);
        if(kind=="audio" && startOk && start==0. && !row.value("start_time").isBool()) {
            auto endpoint=stream.duration;
            if(row.value("duration").isBool())endpoint.reset();
            if(!endpoint)endpoint=taggedAudioDuration(row.value("tags").toObject());
            if(endpoint)audioEnds.insert(*number,*endpoint);
        }
        (kind == "audio" ? audio : video).append(stream);
    }
    const auto order = [](const MediaStream &a, const MediaStream &b) { return a.index < b.index; };
    std::stable_sort(audio.begin(), audio.end(), order); std::stable_sort(video.begin(), video.end(), order);
    const auto *selectedAudio = select(audio, false), *selectedVideo = select(video, true);
    if (!selectedAudio && !selectedVideo) throw DetectionError("no_usable_streams", "media has no usable audio or video streams");
    const auto container = payload.value("format").toObject(); auto length = duration(container.value("duration").toVariant());
    auto source = DurationSource::Container;
    if (!length && selectedVideo && selectedVideo->duration) { length = selectedVideo->duration; source = DurationSource::VideoStream; }
    if (!length && selectedAudio && selectedAudio->duration) { length = selectedAudio->duration; source = DurationSource::AudioStream; }
    if (!length) throw DetectionError("duration_unavailable", "no finite positive media duration is available");
    return {identity, *length, source, container.value("format_name").toString(), audio, video,
        selectedAudio ? std::optional<qint64>(selectedAudio->index) : std::nullopt,
        selectedVideo ? std::optional<qint64>(selectedVideo->index) : std::nullopt,
        selectedAudio && audioEnds.contains(selectedAudio->index) ? std::optional<double>(audioEnds.value(selectedAudio->index)) : std::nullopt};
}

TimeRange audioAnalysisWindow(const MediaInfo &media,const TimeRange &window)
{
    if(!TimeRange(0,media.duration).contains(window))throw std::invalid_argument("Audio window exceeds the media duration.");
    const double end=media.audioEnd?std::min(window.end,*media.audioEnd):window.end;
    if(!std::isfinite(end) || end<=window.start)throw DetectionError("audio_window_unavailable","The selected audio stream ends before this search window.");
    return {window.start,end};
}

MediaInfo inspectMedia(ProcessRunner &runner, const QString &path, const QString &episodeId,
                       const QString &jobId, const Cancellation &cancel, const QString &program, const IdentityReader &identityReader)
{
    const auto identity = identityReader ? identityReader(path) : readMediaIdentity(path);
    const auto result = runner.run({{program, "-v", "error", "-show_format", "-show_streams", "-of", "json", identity.normalizedPath},
        ProcessCategory::Probe, "ffprobe", jobId, episodeId, {}, {}}, cancel);
    if (result.cancelled) throw Cancelled("probe_cancelled", "ffprobe inspection cancelled");
    if (result.outputLimitExceeded) throw DetectionError("process_output_limit", "ffprobe exceeded its output budget");
    if (result.timedOut) throw DetectionError("detector_error", "ffprobe timed out");
    if (result.exitCode != 0) throw DetectionError("detector_error", result.error.trimmed().isEmpty() ? QString("ffprobe failed") : QString::fromUtf8(result.error.trimmed()));
    QJsonParseError error; const auto payload = QJsonDocument::fromJson(result.out, &error);
    if (error.error != QJsonParseError::NoError || !payload.isObject()) throw DetectionError("detector_error", "invalid ffprobe JSON");
    return parseMediaInfo(payload.object(), identity);
}

} // namespace introvert::detection
