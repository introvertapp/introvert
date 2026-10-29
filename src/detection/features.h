#pragma once

#include "audio_decode.h"
#include <vector>

namespace introvert::detection {

class FeatureTrack final {
public:
    FeatureTrack(QString episodeId, SegmentType type, TimeRange window, QString reference,
                 QString extractorId, QString extractorVersion, QString schemaVersion,
                 std::vector<double> timestamps, std::vector<float> values, size_t columns);
    const QString episodeId, reference, extractorId, extractorVersion, schemaVersion;
    const SegmentType type;
    const TimeRange window;
    const std::vector<double> timestamps;
    const std::vector<float> values;
    const size_t columns;
    size_t rows() const { return timestamps.size(); }
};

struct AudioRecurrenceSpecification {
    QString version = "audio-spectral-recurrence-v1";
    int frameSize = 4096, hopSize = 2048, bandCount = 32;
    double minimumFrequency = 80, maximumFrequency = 8000;
    QString windowFunction = "hann", outputDtype = "float32";
    double silenceFloorDbfs = -60;
    int batchSize = 512;
    double logEpsilon = 1e-12, normalizationEpsilon = 1e-12;
    void validate(const AudioDecodeSpecification &decode = {}) const;
};
struct AudioFeatures { FeatureTrack track; QVariantMap diagnostic; };

class AudioSpectralExtractor final {
public:
    explicit AudioSpectralExtractor(AudioRecurrenceSpecification specification = {}, AudioDecodeSpecification decode = {});
    ~AudioSpectralExtractor();
    const std::vector<std::pair<int, int>> &bandRanges() const { return m_ranges; }
    QVariantList configuration() const;
    QVariantMap sourceFailureDiagnostic(const QString &episode, SegmentType type, const QString &code, double seconds) const;
    AudioFeatures extract(const AudioWindow &audio, const Cancellation &cancel = {}, const DiagnosticSink &failureSink = {}) const;
private:
    const AudioRecurrenceSpecification m_spec;
    const AudioDecodeSpecification m_decode;
    std::vector<double> m_hann;
    std::vector<std::pair<int, int>> m_ranges;
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace introvert::detection
