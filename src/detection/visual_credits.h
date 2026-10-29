#pragma once
#include "contracts.h"
#include <QVariantMap>
#include <QByteArray>

namespace introvert::detection {
inline constexpr int visualFeatureColumns = 10;
// Features contain geometry/semantic counts only, never recognized names or text.
enum VisualColumn { TextLines, TextRows, PairedRows, RoleLines, NameLines, TextConfidence, TextArea, TextHeight, DarkFraction, BottomRows };
struct VisualTextLine { double x, y, width, height, confidence; bool role, name; };
QVariantList visualFrameFeatures(const QList<VisualTextLine> &lines, double darkFraction);
QVariantMap visualCreditsDescription();
QVariantList observeCreditFrame(const QByteArray &rgb, int width, int height, const Cancellation &cancel);
bool creditFrame(const QVariantList &features);
bool leadingCreditFrame(const QVariantList &features);
QVariantMap findVisualCredits(const QVariantList &observations, double step, const TimeRange &window, const Cancellation &cancel = {});
QVariantList refineVisualCredits(const QVariantList &candidates, const QVariantList &observations, const TimeRange &window, const Cancellation &cancel = {});
QVariantMap fuseCreditCandidates(const QVariantList &audioSelected, const QVariantList &matches, const QVariantMap &visual, const Cancellation &cancel = {}, double edgeRadius = 5.);
} // namespace introvert::detection
