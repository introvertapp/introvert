#include "audio_levels.h"
#include <algorithm>
#include <cmath>

namespace introvert::editor {
namespace {
template<typename T>
std::array<double, 2> measure(const QAudioBuffer &buffer, const std::array<double, 2> &previous, double scale, double offset)
{
    auto result = previous;
    const auto channels = buffer.format().channelCount();
    const auto *samples = buffer.constData<T>();
    const auto count = buffer.byteCount() / qsizetype(sizeof(T));
    for (int channel = 0; channel < 2; ++channel) {
        double sum = 0;
        qsizetype size = 0;
        for (qsizetype index = std::min(channel, channels - 1); index < count; index += channels) {
            const auto value = (double(samples[index]) - offset) / scale;
            sum += value * value; ++size;
        }
        const auto rms = std::sqrt(sum / std::max<qsizetype>(1, size));
        const auto level = rms > .00001 ? std::clamp((20 * std::log10(rms) + 48) / 48, 0., 1.) : 0.;
        result[channel] = std::max(level, previous[channel] * .88);
    }
    return result;
}
}

std::array<double, 2> audioLevels(const QAudioBuffer &buffer, const std::array<double, 2> &previous)
{
    if (!buffer.isValid() || buffer.format().channelCount() < 1) return previous;
    switch (buffer.format().sampleFormat()) {
    case QAudioFormat::UInt8: return measure<quint8>(buffer, previous, 128., 128.);
    case QAudioFormat::Int16: return measure<qint16>(buffer, previous, 32768., 0.);
    case QAudioFormat::Int32: return measure<qint32>(buffer, previous, 2147483648., 0.);
    case QAudioFormat::Float: return measure<float>(buffer, previous, 1., 0.);
    default: return previous;
    }
}

} // namespace introvert::editor
