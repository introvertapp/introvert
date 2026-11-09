#pragma once

#include "endpoint_client.h"
#include "segments/segment_store.h"

namespace introvert::submissions {

// Reviewed, credential-free canonical snapshot. Worker-only DB reads/imports;
// preparation may verify the episode through the read-only v4 mapping endpoint.
// A multi-endpoint review may skip IntroDB if there are no movie Credits.
QVariantMap prepareSubmission(const providers::ShowDetail &detail, const QVariantList &selection,
                              const QString &endpoint, const SettingsStore &settings, const segments::SegmentStore &segments,
                              const providers::Cancellation &cancelled = {}, const QString &scope = {},
                              bool allowMissingMovieCredits = false);

} // namespace introvert::submissions
