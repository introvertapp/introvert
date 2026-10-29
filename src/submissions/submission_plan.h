#pragma once

#include "endpoint_client.h"
#include "segments/segment_store.h"

namespace introvert::submissions {

// Reviewed, credential-free canonical snapshot. Worker-only DB reads/imports;
// preparation and validation never contact an endpoint.
QVariantMap prepareSubmission(const providers::ShowDetail &detail, const QVariantList &selection,
                              const QString &endpoint, const SettingsStore &settings, const segments::SegmentStore &segments);

} // namespace introvert::submissions
