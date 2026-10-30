#pragma once

#include "contracts.h"
#include <QJsonObject>

namespace introvert::providers {

// Read-only chapter metadata; only explicitly named supported segment types
// become timestamps. Ordinary chapter names never imply a segment.
QList<ImportedSegment> embeddedSegments(const QJsonObject &probe);
// True means a complete metadata read, including a valid empty chapter list.
bool inspectEmbeddedSegments(Episode &episode, const Cancellation &cancelled = {});

} // namespace introvert::providers
