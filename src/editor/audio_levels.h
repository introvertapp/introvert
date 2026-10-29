#pragma once

#include <QAudioBuffer>
#include <array>

namespace introvert::editor {

std::array<double, 2> audioLevels(const QAudioBuffer &buffer, const std::array<double, 2> &previous);

} // namespace introvert::editor
