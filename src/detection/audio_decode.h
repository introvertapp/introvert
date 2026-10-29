#pragma once

#include "process_runner.h"
#include <QVariantMap>
#include <vector>

class QTemporaryDir;

namespace introvert::detection {

struct AudioDecodeSpecification {
    QString version = audioDecodeSpecVersion;
    int sampleRate = 22050, channelCount = 1;
    QString sampleFormat = "s16le", numpyDtype = "<i2";
    int bytesPerFrame = 2;
    void validate() const;
};
struct AudioDecodeTolerance {
    double durationSeconds = .05, mediaEofSeconds = 1e-6;
    void validate() const;
};
QString mediaDecimal(double value);
struct AudioWindow {
    QString episodeId;
    SegmentType type;
    TimeRange requested;
    qint64 streamIndex;
    AudioDecodeSpecification specification;
    QString pcmPath;
    qint64 byteCount, sampleCount;
    double decodedDuration, durationDifference;
    AudioWindow(QString episodeId, SegmentType type, TimeRange requested, qint64 streamIndex,
                AudioDecodeSpecification specification, QString pcmPath, qint64 byteCount, qint64 sampleCount,
                double decodedDuration, double durationDifference);
    double absoluteTime(qint64 sample) const;
    std::vector<qint16> readSamples(qint64 start, qsizetype count) const;
};
class DecodedAudio final {
public:
    DecodedAudio(AudioWindow window, QVariantMap diagnostic, std::unique_ptr<QTemporaryDir> scope);
    ~DecodedAudio();
    DecodedAudio(DecodedAudio &&) noexcept;
    DecodedAudio &operator=(DecodedAudio &&) noexcept;
    const AudioWindow &window() const { return m_window; }
    const QVariantMap &diagnostic() const { return m_diagnostic; }
private:
    AudioWindow m_window;
    QVariantMap m_diagnostic;
    std::unique_ptr<QTemporaryDir> m_scope;
};

QStringList audioDecodeArguments(const QString &mediaPath, qint64 streamIndex, const TimeRange &window,
                                 const QString &pcmPath, const AudioDecodeSpecification &specification = {},
                                 const QString &program = "ffmpeg");
using DiagnosticSink = std::function<void(const QVariantMap &)>;
std::vector<DecodedAudio> decodeAudioVariants(ProcessRunner &runner, const MediaInfo &media, const QString &episodeId, SegmentType type,
    const TimeRange &window, const QString &jobId, const QString &temporaryRoot, const std::vector<AudioDecodeSpecification> &specifications,
    const Cancellation &cancel = {}, const AudioDecodeTolerance &tolerance = {}, double timeoutSeconds = 300,
    const DiagnosticSink &failureSink = {}, const QString &program = "ffmpeg");
DecodedAudio decodeAudio(ProcessRunner &runner, const MediaInfo &media, const QString &episodeId, SegmentType type,
                         const TimeRange &window, const QString &jobId, const QString &temporaryRoot,
                         const Cancellation &cancel = {}, const AudioDecodeSpecification &specification = {},
                         const AudioDecodeTolerance &tolerance = {}, double timeoutSeconds = 300,
                         const DiagnosticSink &failureSink = {}, const QString &program = "ffmpeg");

} // namespace introvert::detection
