#pragma once

#include "process_runner.h"
#include <QJsonObject>

namespace introvert::detection {

QString normalizedMediaPath(const QString &path);
MediaIdentity readMediaIdentity(const QString &path);
MediaInfo parseMediaInfo(const QJsonObject &payload, const MediaIdentity &identity);
TimeRange audioAnalysisWindow(const MediaInfo &media, const TimeRange &window);
using IdentityReader = std::function<MediaIdentity(const QString &)>;
MediaInfo inspectMedia(ProcessRunner &runner, const QString &path, const QString &episodeId,
                       const QString &jobId, const Cancellation &cancel = {}, const QString &program = "ffprobe",
                       const IdentityReader &identityReader = {});

} // namespace introvert::detection
