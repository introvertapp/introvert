#include "algorithm_data.h"
#include <QCborValue>

namespace introvert::detection {
QByteArray algorithmMessage(const QVariantMap &message)
{
    const auto bytes = QCborValue::fromVariant(message).toCbor();
    if (bytes.size() > algorithmMessageLimit) throw DetectionError("algorithm_output_limit", "Algorithm message exceeds its budget.");
    return bytes;
}
QVariantMap parseAlgorithmMessage(const QByteArray &bytes)
{
    if (bytes.isEmpty() || bytes.size() > algorithmMessageLimit) throw DetectionError("algorithm_invalid_message", "Invalid algorithm message size.");
    QCborParserError error; const auto value = QCborValue::fromCbor(bytes, &error);
    if (error.error != QCborError::NoError || !value.isMap()) throw DetectionError("algorithm_invalid_message", "Invalid algorithm message.");
    return value.toVariant().toMap();
}
QVariantMap featureMessage(const FeatureTrack &track)
{
    const auto encoded = encodeFeatures(track);
    return {{"episode", track.episodeId}, {"type", track.type == SegmentType::Intro ? "intro" : "credits"},
        {"start", track.window.start}, {"end", track.window.end}, {"extractor", track.extractorId}, {"version", track.extractorVersion},
        {"schema", track.schemaVersion}, {"timestamps", encoded.timestamps}, {"values", encoded.values}};
}
std::shared_ptr<const FeatureTrack> featureFromMessage(const QVariantMap &m)
{
    const auto kind = m.value("type").toString();
    if ((kind != "intro" && kind != "credits") || m.value("episode").toString().isEmpty()
        || m.value("extractor").toString().isEmpty() || m.value("version").toString().isEmpty() || m.value("schema").toString().isEmpty())
        throw DetectionError("algorithm_invalid_features", "Invalid algorithm feature identity.");
    const auto timestamps = m.value("timestamps").toByteArray(), values = m.value("values").toByteArray();
    if (timestamps.size() + values.size() > algorithmMessageLimit) throw DetectionError("algorithm_output_limit", "Feature message exceeds its budget.");
    const FeatureCacheRequest request{"algorithm", "", 0, 0, m.value("episode").toString(), kind == "intro" ? SegmentType::Intro : SegmentType::Credits,
        {m.value("start").toDouble(), m.value("end").toDouble()}, {}, m.value("extractor").toString(), m.value("version").toString(), m.value("schema").toString(), {}};
    return decodeFeatures(request, {timestamps, values});
}
} // namespace introvert::detection
