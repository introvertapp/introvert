#include "features.h"
#include <cmath>
#include <limits>

namespace introvert::detection {
FeatureTrack::FeatureTrack(QString episode, SegmentType segment, TimeRange range, QString ref,
                           QString extractor, QString version, QString schema, std::vector<double> times,
                           std::vector<float> data, size_t width)
    : episodeId(std::move(episode)), reference(std::move(ref)), extractorId(std::move(extractor)), extractorVersion(std::move(version)),
      schemaVersion(std::move(schema)), type(segment), window(range), timestamps(std::move(times)), values(std::move(data)), columns(width)
{
    if (!columns || rows() > std::numeric_limits<size_t>::max() / columns || values.size() != rows() * columns)
        throw std::invalid_argument("feature timestamps and values must have equal first dimensions");
    double previous = -std::numeric_limits<double>::infinity();
    for (const auto value : timestamps) {
        if (!std::isfinite(value)) throw std::invalid_argument("feature timestamps must be finite");
        if (value <= previous) throw std::invalid_argument("feature timestamps must be strictly increasing");
        if (value < window.start - 1e-9 || value > window.end + 1e-9) throw std::invalid_argument("feature timestamps must remain inside the feature window");
        previous = value;
    }
    for (const auto value : values) if (!std::isfinite(value)) throw std::invalid_argument("floating-point feature values must be finite");
}

} // namespace introvert::detection
