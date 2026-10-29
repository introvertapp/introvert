#pragma once
#include "audio_recurrence.h"

namespace introvert::detection {
using ScopedAudioReader = std::function<std::vector<qint16>(uint32_t, uint32_t, qint64, uint32_t)>;
struct AudioRefinementBudget { qint64 comparisons=3000000; int peaks=16; };
// Fine alignment and local matched-audio transitions, wholly inside the two
// allowed windows. Unresolved edges keep their coarse measured endpoints.
AudioRecurrence refineAudioMatch(const AudioRecurrence &match, const TimeRange &windowA, const TimeRange &windowB,
    const ScopedAudioReader &read, const Cancellation &cancel = {}, AudioRefinementBudget budget = {});
}
