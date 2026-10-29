#include "visual_credits.h"
namespace introvert::detection {
QVariantMap visualCreditsDescription() { return {{"available",false},{"reason","visual_text_backend_unavailable"}}; }
QVariantList observeCreditFrame(const QByteArray &, int, int, const Cancellation &) { throw DetectionError("visual_unavailable","No visual text backend."); }
}
