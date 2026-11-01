#pragma once

#include "audio_recurrence.h"

namespace introvert::detection {

// Season agreement prompts a check, never supplies a missing timestamp. Recovery
// requires coherent, independently verified fragments with measured outer edges.
void checkIntroConsistency(const std::vector<AudioRecurrence> &matches,
    std::vector<AudioSegment> &selected, const Cancellation &cancel, qint64 &work, QVariantList *diagnostics);

// Compare independently aligned season peers, with at most one weighted vote
// per episode. Minority variants remain separate; duration is never a timestamp.
void checkSeasonConsistency(const std::vector<AudioRecurrence> &matches,
    std::vector<AudioSegment> &selected, const Cancellation &cancel, qint64 &work, QVariantList *diagnostics,
    const QVariantList &windows = {},QVariantList *confirmationCandidates = nullptr,
    const std::vector<AudioSegment> *priorSelections = nullptr);

} // namespace introvert::detection
