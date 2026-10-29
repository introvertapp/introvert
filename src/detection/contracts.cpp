#include "contracts.h"
#include <algorithm>
#include <cmath>

namespace introvert::detection {

TimeRange::TimeRange(double first, double last) : start(first), end(last)
{
    if (!std::isfinite(start) || !std::isfinite(end)) throw std::invalid_argument("time range bounds must be finite");
    if (start < 0 || start >= end) throw std::invalid_argument("time range must satisfy 0 <= start < end");
}

QList<ScanWindow> planWindows(double duration, const QList<SegmentType> &types)
{
    if (!std::isfinite(duration) || duration <= 0) throw std::invalid_argument("duration must be finite and positive");
    const auto intro = duration < 1800 ? 300. : 600.;
    const auto credits = duration < 1800 ? 180. : 600.;
    QList<ScanWindow> windows;
    for (const auto type : types) windows.append({type, type == SegmentType::Intro ? TimeRange(0, std::min(intro, duration))
        : TimeRange(std::max(0., duration - credits), duration)});
    return windows;
}

bool MediaInfo::hasUsableStreams() const
{
    return std::any_of(audioStreams.cbegin(), audioStreams.cend(), [&](const MediaStream &stream) { return selectedAudio && stream.index == *selectedAudio; })
        || std::any_of(videoStreams.cbegin(), videoStreams.cend(), [&](const MediaStream &stream) { return selectedVideo && stream.index == *selectedVideo && !stream.isAttachedPicture; });
}

} // namespace introvert::detection
