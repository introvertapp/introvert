#pragma once

#include "providers/contracts.h"
#include <QJsonValue>

namespace introvert::library {

// Explicit cache allowlist: never serialize credentials, item links, remote
// filesystem paths, Local metadata, canonical edits or deletion records.
QJsonValue encodeSeries(const QList<providers::ShowSummary> &shows);
QJsonValue encodeDetail(const providers::ShowDetail &detail);
QList<providers::ShowSummary> decodeSeries(const QJsonValue &value);
providers::ShowDetail decodeDetail(const QJsonValue &value);
QByteArray asciiJson(const QJsonValue &value);

} // namespace introvert::library
