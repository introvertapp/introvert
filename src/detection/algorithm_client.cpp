#include "algorithm_client.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <cstring>
#include <exception>

namespace introvert::detection {
namespace {
struct Call {
    Cancellation cancel;
    Algorithm::AudioReader read;
    QByteArray output;
    std::exception_ptr exception;
    bool emitted = false, invalidOutput = false;
};
int cancelled(void *context)
{
    try { checkCancelled(static_cast<Call *>(context)->cancel); return 0; } catch (...) { return 1; }
}
int64_t readPcm(void *context, uint32_t source, uint32_t rate, int64_t first, uint32_t count, int16_t *output)
{
    auto &call = *static_cast<Call *>(context);
    try {
        checkCancelled(call.cancel);
        if (!call.read || !output || source > 1 || rate < 8000 || rate > 96000 || first < 0 || count == 0 || count > 262144)
            throw DetectionError("algorithm_invalid_audio_read", "Invalid scoped PCM read.");
        const auto samples = call.read(source, rate, first, count);
        if (samples.size() > count) throw DetectionError("algorithm_incomplete_audio_read", "Scoped PCM read exceeded its request.");
        if (!samples.empty()) std::memcpy(output, samples.data(), samples.size() * sizeof(int16_t)); return int64_t(samples.size());
    } catch (...) { call.exception = std::current_exception(); return -1; }
}
int emitResult(void *context, const uint8_t *data, uint64_t length)
{
    auto &call = *static_cast<Call *>(context);
    try {
        if (call.emitted || !data || length == 0 || length > uint64_t(algorithmMessageLimit)) { call.invalidOutput = true; return 0; }
        call.emitted = true; call.output = QByteArray(reinterpret_cast<const char *>(data), qsizetype(length)); return 1;
    } catch (...) { call.exception = std::current_exception(); return 0; }
}
}
Algorithm::Algorithm(const AlgorithmPackage &package) : m_library(package.library)
{
    try {
    m_library.setLoadHints(QLibrary::ResolveAllSymbolsHint);
    if (!m_library.load()) throw DetectionError("algorithm_load_failed", "Cannot load the detector algorithm or its runtime.");
    const auto entry = reinterpret_cast<IntrovertDetectorEntryV1>(m_library.resolve("introvert_detector_api_v1"));
    if (!entry || !(m_api = entry()) || m_api->size < sizeof(IntrovertDetectorApiV1) || m_api->interface_version != algorithmInterfaceVersion || !m_api->metadata || !m_api->invoke)
        throw DetectionError("algorithm_incompatible", "The detector algorithm interface is incompatible.");
    const auto raw = m_api->metadata(); if (!raw) throw DetectionError("algorithm_invalid_metadata", "Missing algorithm metadata.");
    const auto count = qstrnlen(raw, 65537); if (count > 262144) throw DetectionError("algorithm_invalid_metadata", "Algorithm metadata is too large.");
    const auto document = QJsonDocument::fromJson(QByteArray(raw, qsizetype(count)));
    if (!document.isObject()) throw DetectionError("algorithm_invalid_metadata", "Invalid algorithm metadata.");
    m_metadata = document.object().toVariantMap();
    for (const auto &key : {"id", "version", "interface_version", "result_schema_version", "qt_version", "minimum_app_version", "window_policy", "composition", "selection"})
        if (m_metadata.value(key) != package.manifest.value(key)) throw DetectionError("algorithm_identity_mismatch", "Algorithm metadata does not match its package.");
    } catch (...) { m_library.unload(); throw; }
}
Algorithm::~Algorithm() { m_library.unload(); }
QVariantMap Algorithm::invoke(const char *operation, const QVariantMap &request, const Cancellation &cancel, const AudioReader &read, QVariantMap *failureStatistics) const
{
    checkCancelled(cancel); const auto input = algorithmMessage(request); Call call{cancel, read, {}, {}, false, false};
    const IntrovertDetectorHostV1 host{sizeof(IntrovertDetectorHostV1), algorithmInterfaceVersion, &call, cancelled, readPcm, emitResult};
    const int returned = m_api->invoke(operation, reinterpret_cast<const uint8_t *>(input.constData()), uint64_t(input.size()), &host);
    if (call.exception) std::rethrow_exception(call.exception);
    checkCancelled(cancel);
    if (!returned || !call.emitted || call.invalidOutput) throw DetectionError("algorithm_invalid_result", "The detector algorithm returned an invalid result.");
    const auto response = parseAlgorithmMessage(call.output);
    if (response.value("ok").metaType().id() != QMetaType::Bool) throw DetectionError("algorithm_invalid_result", "The algorithm result has no outcome.");
    if (!response.value("ok").toBool()) {
        if (failureStatistics) *failureStatistics = response.value("statistics").toMap();
        const auto reported = response.value("code").toString();
        const auto code = reported.size() <= 80 && QRegularExpression("^[a-z0-9_]{1,80}$").match(reported).hasMatch() ? reported : QString("algorithm_failed");
        if (code == "cancelled") throw Cancelled();
        throw DetectionError(code, "The detector algorithm could not complete this operation.");
    }
    if (response.value("value").metaType().id() != QMetaType::QVariantMap) throw DetectionError("algorithm_invalid_result", "Invalid algorithm result payload.");
    return response.value("value").toMap();
}
QVariantMap Algorithm::description(const QVariantMap &parameters,const QString &effort) const
{
    QVariantMap request{{"parameters",parameters}};if(!effort.isEmpty())request["effort"]=effort;
    const auto d = invoke("describe", request);
    const int rate = d.value("sample_rate").toInt(), hop = d.value("hop_samples").toInt(), context = d.value("context_samples").toInt(), columns = d.value("columns").toInt();
    if (rate < 8000 || rate > 96000 || hop <= 0 || context < hop || context > rate * 60 || columns < 1 || columns > 1024
        || d.value("extractor").toString().isEmpty() || d.value("extractor_version").toString().isEmpty() || d.value("schema").toString().isEmpty())
        throw DetectionError("algorithm_invalid_description", "Invalid algorithm audio feature contract.");
    return d;
}
} // namespace introvert::detection
