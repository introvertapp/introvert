#include "audio_decode.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtEndian>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>

namespace introvert::detection {
QString mediaDecimal(double value)
{
    char buffer[512]; const auto result = std::to_chars(std::begin(buffer), std::end(buffer), value, std::chars_format::fixed, 6);
    if (result.ec != std::errc()) throw std::invalid_argument("invalid audio seek time");
    auto text = QString::fromLatin1(buffer, result.ptr - buffer);
    while (text.endsWith('0')) text.chop(1);
    if (text.endsWith('.')) text.chop(1);
    return text.isEmpty() ? QString("0") : text;
}

void AudioDecodeSpecification::validate() const
{
    if (version.isEmpty() || sampleRate <= 0 || channelCount <= 0) throw std::invalid_argument("audio decode specification values must be positive");
    if (channelCount != 1 || sampleFormat != "s16le") throw std::invalid_argument("baseline audio decoding must use mono signed 16-bit PCM");
    if (!QStringList{"<i2", "int16", "i2", "short", "h", "<h"}.contains(numpyDtype))
        throw std::invalid_argument("baseline audio decode dtype must be signed 16-bit little-endian");
    if (bytesPerFrame != 2) throw std::invalid_argument("audio sample-frame size does not match the decode specification");
}
void AudioDecodeTolerance::validate() const
{
    if (!std::isfinite(durationSeconds) || !std::isfinite(mediaEofSeconds) || durationSeconds < 0 || mediaEofSeconds < 0)
        throw std::invalid_argument("audio decode tolerances must not be negative");
}
AudioWindow::AudioWindow(QString episode, SegmentType segment, TimeRange range, qint64 stream, AudioDecodeSpecification spec,
                         QString path, qint64 bytes, qint64 samples, double decoded, double difference)
    : episodeId(std::move(episode)), type(segment), requested(range), streamIndex(stream), specification(std::move(spec)),
      pcmPath(std::move(path)), byteCount(bytes), sampleCount(samples), decodedDuration(decoded), durationDifference(difference)
{
    specification.validate();
    if (streamIndex < 0 || byteCount <= 0 || sampleCount <= 0) throw std::invalid_argument("audio window stream and PCM counts must be positive");
    if (sampleCount > std::numeric_limits<qint64>::max() / specification.bytesPerFrame || byteCount != sampleCount * specification.bytesPerFrame)
        throw std::invalid_argument("audio byte and sample counts are inconsistent");
}
double AudioWindow::absoluteTime(qint64 sample) const
{
    if (sample < 0 || sample >= sampleCount) throw std::out_of_range("audio sample index is outside the decoded window");
    return requested.start + double(sample) / specification.sampleRate;
}
std::vector<qint16> AudioWindow::readSamples(qint64 start, qsizetype count) const
{
    if (start < 0 || count <= 0 || start > sampleCount || count > sampleCount - start || count > std::numeric_limits<qsizetype>::max() / 2)
        throw std::out_of_range("audio sample block is outside the decoded window");
    QFile file(pcmPath);
    if (!file.open(QIODevice::ReadOnly) || !file.seek(start * 2)) throw std::runtime_error("Decoded PCM could not be read.");
    const auto bytes = file.read(count * 2);
    if (bytes.size() != count * 2) throw std::runtime_error("Decoded PCM could not be read.");
    std::vector<qint16> values(size_t(count), qint16{});
    for (qsizetype i = 0; i < count; ++i) values[size_t(i)] = qFromLittleEndian<qint16>(bytes.constData() + i * 2);
    return values;
}

DecodedAudio::DecodedAudio(AudioWindow window, QVariantMap diagnostic, std::unique_ptr<QTemporaryDir> scope)
    : m_window(std::move(window)), m_diagnostic(std::move(diagnostic)), m_scope(std::move(scope)) {}
DecodedAudio::~DecodedAudio() = default;
DecodedAudio::DecodedAudio(DecodedAudio &&) noexcept = default;
DecodedAudio &DecodedAudio::operator=(DecodedAudio &&) noexcept = default;

QStringList audioDecodeArguments(const QString &path, qint64 stream, const TimeRange &window, const QString &pcm,
                                 const AudioDecodeSpecification &spec, const QString &program)
{
    spec.validate(); const auto fast = std::max(0., window.start - 5.);
    return {program, "-hide_banner", "-nostdin", "-loglevel", "error", "-y", "-ss", mediaDecimal(fast), "-i", path,
        "-ss", mediaDecimal(window.start - fast), "-map", "0:" + QString::number(stream), "-t", mediaDecimal(window.duration()),
        "-vn", "-sn", "-dn", "-ac", QString::number(spec.channelCount), "-ar", QString::number(spec.sampleRate),
        "-c:a", "pcm_s16le", "-f", spec.sampleFormat, pcm};
}

DecodedAudio decodeAudio(ProcessRunner &runner, const MediaInfo &media, const QString &episode, SegmentType type,
                         const TimeRange &window, const QString &job, const QString &root, const Cancellation &cancel,
                         const AudioDecodeSpecification &spec, const AudioDecodeTolerance &tolerance, double timeout,
                         const DiagnosticSink &failureSink, const QString &program)
{
    spec.validate(); tolerance.validate();
    const double expected = std::floor(window.duration() * spec.sampleRate + .5);
    if (expected > double(std::numeric_limits<qint64>::max() / 2)) throw std::invalid_argument("audio window is too large");
    qint64 byteCount = 0, sampleCount = 0; double runtime = 0, decoded = 0;
    const bool eof = std::abs(window.end - media.duration) <= tolerance.mediaEofSeconds;
    const auto fast = std::max(0., window.start - 5.);
    const auto diagnostic = [&](const QString &state, const QVariant &failure, bool allowance = false) {
        return QVariantMap{{"episode_id", episode}, {"segment_type", type == SegmentType::Intro ? "intro" : "credits"},
            {"decoder_id", "ffmpeg-audio-window"}, {"decoder_version", audioDecoderVersion}, {"decode_spec_version", spec.version},
            {"requested_start_seconds", window.start}, {"requested_end_seconds", window.end}, {"fast_seek_seconds", fast}, {"fine_seek_seconds", window.start - fast},
            {"selected_stream_index", media.selectedAudio ? QVariant(*media.selectedAudio) : QVariant()}, {"sample_rate", spec.sampleRate}, {"channel_count", spec.channelCount},
            {"sample_format", spec.sampleFormat}, {"byte_count", byteCount}, {"sample_count", sampleCount}, {"expected_sample_count", qint64(expected)},
            {"actual_sample_count", sampleCount}, {"sample_count_difference", sampleCount - qint64(expected)}, {"requested_duration_seconds", window.duration()},
            {"decoded_duration_seconds", decoded}, {"duration_difference_seconds", decoded - window.duration()}, {"ends_at_media_eof", eof},
            {"eof_shortfall_allowance_used", allowance}, {"runtime_seconds", runtime}, {"consumer_count", 0}, {"state", state}, {"failure_code", failure}};
    };
    const auto fail = [&](const QString &code, const QString &message, const QString &state = "failure") -> void {
        if (failureSink) failureSink(diagnostic(state, code));
        if (state == "cancelled") throw Cancelled(code, message);
        throw DetectionError(code, message);
    };
    if (!media.selectedAudio) fail("audio_stream_unavailable", "planned media has no selected audio stream");
    const QFileInfo rootInfo(root);
    if (!QDir().mkpath(root) || rootInfo.isSymLink()) throw std::invalid_argument("temporary root must be a real directory");
    const auto canonicalRoot = QFileInfo(root).canonicalFilePath();
    if (canonicalRoot.isEmpty() || !QFileInfo(canonicalRoot).isDir()) throw std::runtime_error("Cannot access temporary audio storage.");
    auto scope = std::make_unique<QTemporaryDir>(canonicalRoot + "/scope-XXXXXX");
    if (!scope->isValid()) throw std::runtime_error("Cannot create temporary audio storage.");
    const auto pcm = scope->path() + "/audio.pcm";
    ProcessResult result;
    try {
        result = runner.run({audioDecodeArguments(media.identity.normalizedPath, *media.selectedAudio, window, pcm, spec, program),
            ProcessCategory::Decoder, "ffmpeg-audio-window", job, episode, type, timeout}, cancel);
    } catch (const Cancelled &) {
        if (failureSink) failureSink(diagnostic("cancelled", "audio_decode_cancelled"));
        throw;
    }
    catch (...) { fail("audio_decode_failed", "FFmpeg audio process could not be launched"); }
    runtime = result.runtimeSeconds;
    if (result.cancelled) fail("audio_decode_cancelled", "audio decode cancelled", "cancelled");
    if (result.outputLimitExceeded) fail("process_output_limit", "FFmpeg exceeded its output budget");
    if (result.timedOut) fail("audio_decode_timeout", "audio decode timed out", "timeout");
    if (result.exitCode != 0) fail("audio_decode_failed", result.error.trimmed().isEmpty() ? QString("FFmpeg audio decode failed") : QString::fromUtf8(result.error.trimmed()));
    byteCount = QFileInfo(pcm).isFile() ? QFileInfo(pcm).size() : 0;
    if (!byteCount) fail("empty_audio_output", "FFmpeg produced no PCM samples");
    if (byteCount % spec.bytesPerFrame) fail("unexpected_pcm_size", "PCM byte count is not divisible by its sample-frame size");
    sampleCount = byteCount / spec.bytesPerFrame; decoded = double(sampleCount) / spec.sampleRate;
    const auto difference = sampleCount - qint64(expected);
    const auto allowed = std::ceil(tolerance.durationSeconds * spec.sampleRate);
    if (difference > allowed) fail("unexpected_pcm_duration", "decoded PCM materially exceeds the requested window");
    if (difference < -allowed && !eof) fail("unexpected_pcm_duration", "decoded PCM is materially shorter than a non-EOF requested window");
    AudioWindow audio{episode, type, window, *media.selectedAudio, spec, pcm, byteCount, sampleCount, decoded, decoded - window.duration()};
    return {std::move(audio), diagnostic("success", {}, difference < -allowed && eof), std::move(scope)};
}

std::vector<DecodedAudio> decodeAudioVariants(ProcessRunner &runner,const MediaInfo &media,const QString &episode,SegmentType type,
    const TimeRange &window,const QString &job,const QString &root,const std::vector<AudioDecodeSpecification> &specifications,
    const Cancellation &cancel,const AudioDecodeTolerance &tolerance,double timeout,const DiagnosticSink &failureSink,const QString &program)
{
    if (specifications.empty() || specifications.size()>4) throw std::invalid_argument("Invalid number of audio outputs.");
    if (specifications.size()==1) {
        std::vector<DecodedAudio> result;
        result.push_back(decodeAudio(runner,media,episode,type,window,job,root,cancel,specifications.front(),tolerance,timeout,failureSink,program)); return result;
    }
    for (const auto &spec:specifications) spec.validate(); tolerance.validate();
    if (!media.selectedAudio) throw DetectionError("audio_stream_unavailable","Media has no usable audio stream.");
    if (!QDir().mkpath(root) || QFileInfo(root).isSymLink()) throw std::invalid_argument("temporary root must be a real directory");
    QTemporaryDir stage(QFileInfo(root).canonicalFilePath()+"/outputs-XXXXXX");
    if (!stage.isValid()) throw std::runtime_error("Cannot create temporary audio storage.");
    QStringList paths,arguments;
    for (size_t i=0;i<specifications.size();++i) {
        const auto path=stage.path()+"/audio-"+QString::number(i)+".pcm"; paths.append(path);
        const auto output=audioDecodeArguments(media.identity.normalizedPath,*media.selectedAudio,window,path,specifications[i],program);
        // Keep each output's original fine seek, channel mix and resampler.
        arguments.append(i==0?output:output.mid(output.indexOf("-i")+2));
    }
    ProcessResult result;
    try {result=runner.run({arguments,ProcessCategory::Decoder,"ffmpeg-audio-window",job,episode,type,timeout},cancel);}
    catch(const Cancelled &){throw;}
    catch(...){throw DetectionError("audio_decode_failed","FFmpeg audio process could not be launched.");}
    class PreparedOutput final : public ProcessRunner {
    public:
        QString path; ProcessResult result;
        ProcessResult run(const ProcessRequest &request,const Cancellation &cancel) override {
            checkCancelled(cancel);
            if (result.exitCode==0 && !result.timedOut && !result.cancelled && QFileInfo::exists(path)
                && !QFile::rename(path,request.arguments.last())) throw DetectionError("audio_output_move_failed","Cannot retain scoped PCM output.");
            return result;
        }
    } prepared;
    prepared.result=result; std::vector<DecodedAudio> outputs;
    for (size_t i=0;i<specifications.size();++i) {
        prepared.path=paths[int(i)];
        // Reuse the established validation and job-owned lifetime, moving files
        // on the same filesystem rather than copying PCM or bypassing checks.
        outputs.push_back(decodeAudio(prepared,media,episode,type,window,job,root,cancel,specifications[i],tolerance,timeout,failureSink,program));
    }
    return outputs;
}

} // namespace introvert::detection
