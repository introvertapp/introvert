#include "features.h"
#include "numerics.h"
#include <QElapsedTimer>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace introvert::detection {
void AudioRecurrenceSpecification::validate(const AudioDecodeSpecification &decode) const
{
    decode.validate();
    if (version.isEmpty() || frameSize <= 0 || hopSize <= 0 || hopSize > frameSize || bandCount < 2
        || !std::isfinite(minimumFrequency) || !std::isfinite(maximumFrequency) || minimumFrequency <= 0
        || maximumFrequency <= minimumFrequency || maximumFrequency > std::min(11025., decode.sampleRate / 2.)
        || windowFunction != "hann" || !QStringList{"float32", "f4", "<f4", "=f4", "single", "f", "<f", "=f"}.contains(outputDtype) || !std::isfinite(silenceFloorDbfs) || batchSize <= 0
        || !std::isfinite(logEpsilon) || logEpsilon <= 0 || !std::isfinite(normalizationEpsilon) || normalizationEpsilon <= 0)
        throw DetectionError("audio_recurrence_invalid_configuration", "invalid audio recurrence specification");
}
struct AudioSpectralExtractor::Impl { RealFft fft; explicit Impl(size_t n) : fft(n) {} };
AudioSpectralExtractor::AudioSpectralExtractor(AudioRecurrenceSpecification spec, AudioDecodeSpecification decode)
    : m_spec(std::move(spec)), m_decode(std::move(decode))
{
    m_spec.validate(m_decode);
    const auto n = m_spec.frameSize;
    std::vector<double> frequencies(size_t(n / 2 + 1));
    const auto step = 1. / (n * (1. / m_decode.sampleRate));
    for (size_t i = 0; i < frequencies.size(); ++i) frequencies[i] = i * step;
    const auto first = int(std::lower_bound(frequencies.begin(), frequencies.end(), m_spec.minimumFrequency) - frequencies.begin());
    const auto last = int(std::upper_bound(frequencies.begin(), frequencies.end(), m_spec.maximumFrequency) - frequencies.begin());
    if (last - first < m_spec.bandCount) throw DetectionError("audio_recurrence_invalid_configuration", "frequency range cannot provide the requested usable bands");
    int previous = first;
    const auto a = std::log10(m_spec.minimumFrequency), increment = (std::log10(m_spec.maximumFrequency) - a) / m_spec.bandCount;
    for (int i = 1; i < m_spec.bandCount; ++i) {
        const auto edge = std::pow(10., a + i * increment);
        const auto raw = int(std::lower_bound(frequencies.begin(), frequencies.end(), edge) - frequencies.begin());
        const auto next = std::clamp(raw, previous + 1, last - (m_spec.bandCount - i));
        m_ranges.emplace_back(previous, next); previous = next;
    }
    m_ranges.emplace_back(previous, last);
    m_hann.resize(size_t(n)); const double pi = std::acos(-1.);
    for (int i = 0; i < n; ++i) m_hann[size_t(i)] = n == 1 ? 1 : .5 + .5 * std::cos(pi * (1. - n + 2. * i) / (n - 1));
    m_impl = std::make_unique<Impl>(size_t(n));
}
AudioSpectralExtractor::~AudioSpectralExtractor() = default;
QVariantList AudioSpectralExtractor::configuration() const
{
    QVariantList result;
    const auto add = [&](const QString &key, const QVariant &value) { result.append(QVariant(QVariantList{key, value})); };
    add("audio_recurrence_feature_version", m_spec.version); add("audio_recurrence_frame_size", m_spec.frameSize);
    add("audio_recurrence_hop_size", m_spec.hopSize); add("audio_recurrence_band_count", m_spec.bandCount);
    add("audio_recurrence_minimum_frequency_hz", m_spec.minimumFrequency); add("audio_recurrence_maximum_frequency_hz", m_spec.maximumFrequency);
    add("audio_recurrence_window_function", m_spec.windowFunction); add("audio_recurrence_output_dtype", m_spec.outputDtype);
    add("audio_recurrence_silence_floor_dbfs", m_spec.silenceFloorDbfs); add("audio_recurrence_batch_size", m_spec.batchSize);
    add("audio_recurrence_log_epsilon", m_spec.logEpsilon); add("audio_recurrence_normalization_epsilon", m_spec.normalizationEpsilon);
    add("audio_decode_spec_version", m_decode.version); add("audio_sample_rate", m_decode.sampleRate); add("audio_channel_count", m_decode.channelCount);
    add("audio_sample_format", m_decode.sampleFormat); add("audio_numpy_dtype", m_decode.numpyDtype); add("audio_bytes_per_sample_frame", m_decode.bytesPerFrame);
    return result;
}

QVariantMap AudioSpectralExtractor::sourceFailureDiagnostic(const QString &episode, SegmentType type, const QString &code, double seconds) const
{
    return {{"episode_id", episode}, {"segment_type", type == SegmentType::Intro ? "intro" : "credits"}, {"extractor_id", "audio-spectral-recurrence"}, {"extractor_version", "audio-spectral-recurrence-v1"}, {"feature_schema_version", "1"},
        {"recurrence_spec_version", m_spec.version}, {"audio_decode_spec_version", m_decode.version}, {"sample_rate", m_decode.sampleRate}, {"frame_size", m_spec.frameSize}, {"hop_size", m_spec.hopSize},
        {"frame_duration_seconds", double(m_spec.frameSize) / m_decode.sampleRate}, {"hop_duration_seconds", double(m_spec.hopSize) / m_decode.sampleRate}, {"band_count", m_spec.bandCount},
        {"minimum_frequency_hz", m_spec.minimumFrequency}, {"maximum_frequency_hz", m_spec.maximumFrequency}, {"complete_frame_count", 0}, {"silent_frame_count", 0},
        {"discarded_tail_sample_count", 0}, {"decoded_sample_count", 0}, {"batch_count", 0}, {"runtime_seconds", seconds}, {"empty_track", true}, {"state", "failure"}, {"failure_code", code}};
}

AudioFeatures AudioSpectralExtractor::extract(const AudioWindow &audio, const Cancellation &cancel, const DiagnosticSink &failureSink) const
{
    QElapsedTimer timer; timer.start();
    const auto analysis = std::min(audio.sampleCount, qint64(std::floor(audio.requested.duration() * m_decode.sampleRate + .5)));
    const qint64 count = analysis < m_spec.frameSize ? 0 : 1 + (analysis - m_spec.frameSize) / m_spec.hopSize;
    const qint64 used = count ? (count - 1) * m_spec.hopSize + m_spec.frameSize : 0;
    qint64 silent = 0, batches = 0;
    const auto diagnostic = [&](const QString &state, const QVariant &failure) {
        return QVariantMap{{"episode_id", audio.episodeId}, {"segment_type", audio.type == SegmentType::Intro ? "intro" : "credits"},
            {"extractor_id", "audio-spectral-recurrence"}, {"extractor_version", "audio-spectral-recurrence-v1"}, {"feature_schema_version", "1"},
            {"recurrence_spec_version", m_spec.version}, {"audio_decode_spec_version", m_decode.version}, {"sample_rate", m_decode.sampleRate},
            {"frame_size", m_spec.frameSize}, {"hop_size", m_spec.hopSize}, {"frame_duration_seconds", double(m_spec.frameSize) / m_decode.sampleRate},
            {"hop_duration_seconds", double(m_spec.hopSize) / m_decode.sampleRate}, {"band_count", m_spec.bandCount}, {"minimum_frequency_hz", m_spec.minimumFrequency},
            {"maximum_frequency_hz", m_spec.maximumFrequency}, {"complete_frame_count", count}, {"silent_frame_count", silent},
            {"discarded_tail_sample_count", audio.sampleCount - used}, {"decoded_sample_count", audio.sampleCount}, {"batch_count", batches},
            {"runtime_seconds", timer.nsecsElapsed() / 1e9}, {"empty_track", count == 0}, {"state", state}, {"failure_code", failure}};
    };
    std::vector<double> times; std::vector<float> values;
    try {
        if (quint64(count) > std::numeric_limits<size_t>::max() / size_t(m_spec.bandCount)) throw std::length_error("feature track too large");
        times.resize(size_t(count)); values.resize(size_t(count) * size_t(m_spec.bandCount));
        std::vector<double> frame(size_t(m_spec.frameSize)), power(size_t(m_spec.frameSize / 2 + 1)), logged(size_t(m_spec.bandCount));
        std::vector<std::complex<double>> spectrum;
        const auto floor = std::pow(10., m_spec.silenceFloorDbfs / 20.);
        for (qint64 batch = 0; batch < count; batch += m_spec.batchSize) {
            checkCancelled(cancel);
            const auto end = std::min(count, batch + m_spec.batchSize);
            const auto samples = audio.readSamples(batch * m_spec.hopSize, qsizetype((end - batch - 1) * m_spec.hopSize + m_spec.frameSize));
            for (qint64 index = batch; index < end; ++index) {
                const auto offset = size_t((index - batch) * m_spec.hopSize); double rms = 0;
                for (size_t i = 0; i < frame.size(); ++i) {
                    const double value = samples[offset + i] / 32768.; rms += value * value; frame[i] = value * m_hann[i];
                }
                const bool quiet = std::sqrt(rms / m_spec.frameSize) < floor; silent += quiet;
                m_impl->fft.forward(frame, spectrum);
                for (size_t i = 0; i < power.size(); ++i) power[i] = spectrum[i].real() * spectrum[i].real() + spectrum[i].imag() * spectrum[i].imag();
                double mean = 0;
                for (size_t band = 0; band < m_ranges.size(); ++band) {
                    double sum = 0; for (int bin = m_ranges[band].first; bin < m_ranges[band].second; ++bin) sum += power[size_t(bin)];
                    logged[band] = std::log(sum + m_spec.logEpsilon); mean += logged[band];
                }
                mean /= m_spec.bandCount; double norm = 0;
                for (auto &value : logged) { value -= mean; norm += value * value; }
                norm = std::sqrt(norm);
                for (size_t band = 0; band < logged.size(); ++band)
                    values[size_t(index) * logged.size() + band] = quiet || norm <= m_spec.normalizationEpsilon ? 0.f : float(logged[band] / std::max(norm, m_spec.normalizationEpsilon));
                times[size_t(index)] = audio.requested.start + double(index * m_spec.hopSize) / m_decode.sampleRate;
            }
            ++batches;
        }
        return {FeatureTrack(audio.episodeId, audio.type, audio.requested,
            "audio-recurrence:" + audio.episodeId + ':' + (audio.type == SegmentType::Intro ? "intro" : "credits"),
            "audio-spectral-recurrence", "audio-spectral-recurrence-v1", "1", std::move(times), std::move(values), size_t(m_spec.bandCount)),
            diagnostic("success", {})};
    } catch (const Cancelled &error) {
        if (failureSink) failureSink(diagnostic("failure", error.code)); throw;
    } catch (const DetectionError &error) {
        if (failureSink) failureSink(diagnostic("failure", error.code)); throw;
    } catch (const std::runtime_error &) {
        if (failureSink) failureSink(diagnostic("failure", "audio_recurrence_invalid_pcm"));
        throw DetectionError("audio_recurrence_invalid_pcm", "audio recurrence PCM could not be read");
    } catch (...) {
        if (failureSink) failureSink(diagnostic("failure", "audio_recurrence_analysis_failed"));
        throw DetectionError("audio_recurrence_analysis_failed", "audio recurrence analysis failed");
    }
}
} // namespace introvert::detection
