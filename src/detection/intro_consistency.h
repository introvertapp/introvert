#pragma once

#include "audio_recurrence.h"

namespace introvert::detection {

// Season agreement prompts a check, never supplies a missing timestamp. Recovery
// requires coherent, independently verified fragments with measured outer edges.
void checkIntroConsistency(const std::vector<AudioRecurrence> &matches,
    std::vector<AudioSegment> &selected, const Cancellation &cancel, qint64 &work, QVariantList *diagnostics);

} // namespace introvert::detection
