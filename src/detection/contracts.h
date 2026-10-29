#pragma once

#include <QList>
#include <QString>
#include <functional>
#include <optional>
#include <stdexcept>

namespace introvert::detection {

inline constexpr char windowPolicyVersion[] = "duration-policy-v1";
inline constexpr char mediaProbeVersion[] = "ffprobe-media-probe-v2";
inline constexpr char resourceManagerVersion[] = "1";
inline constexpr char audioDecodeSpecVersion[] = "audio-pcm-v1";
inline constexpr char audioDecoderVersion[] = "ffmpeg-audio-window-v2";

class DetectionError : public std::runtime_error {
public:
    DetectionError(QString code, const QString &message) : std::runtime_error(message.toStdString()), code(std::move(code)) {}
    const QString code;
};
class Cancelled final : public DetectionError {
public:
    explicit Cancelled(const QString &code = "cancelled", const QString &message = "job cancelled") : DetectionError(code, message) {}
};
using Cancellation = std::function<void()>;
inline void checkCancelled(const Cancellation &cancel) { if (cancel) cancel(); }

enum class SegmentType { Intro, Credits };
enum class DurationSource { Container, VideoStream, AudioStream };
enum class ProcessCategory { Probe, Decoder };

struct TimeRange {
    double start, end;
    TimeRange(double start, double end);
    double duration() const { return end - start; }
    bool contains(const TimeRange &other) const { return start <= other.start && other.end <= end; }
};
struct ScanWindow { SegmentType type; TimeRange range; };
QList<ScanWindow> planWindows(double duration, const QList<SegmentType> &types);

struct MediaIdentity { QString normalizedPath; qint64 fileSize, modifiedTimeNs; };
struct MediaStream {
    qint64 index;
    QString kind;
    std::optional<double> duration;
    bool isDefault = false, isAttachedPicture = false;
};
struct MediaInfo {
    MediaIdentity identity;
    double duration;
    DurationSource durationSource;
    QString containerFormat;
    QList<MediaStream> audioStreams, videoStreams;
    std::optional<qint64> selectedAudio, selectedVideo;
    // An independently declared audio endpoint on a zero-origin stream. It
    // bounds analysis only; container duration still defines hard windows/EOF.
    std::optional<double> audioEnd;
    bool hasUsableStreams() const;
};
class EpisodePlan final {
public:
    EpisodePlan(QString episodeId, MediaInfo media, QList<SegmentType> requested, QList<ScanWindow> windows)
        : episodeId(std::move(episodeId)), media(std::move(media)), requested(std::move(requested)), windows(std::move(windows)) {}
    const QString episodeId;
    const MediaInfo media;
    const QList<SegmentType> requested;
    const QList<ScanWindow> windows;
    const ScanWindow &windowFor(SegmentType type) const
    {
        for (const auto &window : windows) if (window.type == type) return window;
        throw std::out_of_range("requested scan window is unavailable");
    }
};

} // namespace introvert::detection
