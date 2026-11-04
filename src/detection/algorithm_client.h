#pragma once
#include "algorithm_api.h"
#include "algorithm_data.h"
#include "algorithm_package.h"
#include "algorithm_session.h"
#include <QLibrary>

namespace introvert::detection {
// Job-owned library handle. Updating the selected package cannot change a
// running job, its code, rules or dependencies.
class Algorithm final {
public:
    using AudioReader = std::function<std::vector<qint16>(uint32_t source, uint32_t rate, qint64 first, uint32_t count)>;
    using AudioReaderInto = std::function<int64_t(uint32_t source, uint32_t rate, qint64 first, uint32_t count, int16_t *output)>;
    explicit Algorithm(const AlgorithmPackage &package);
    Algorithm(const IntrovertDetectorApiV1 *api, QVariantMap metadata);
    ~Algorithm();
    Algorithm(const Algorithm &) = delete;
    Algorithm &operator=(const Algorithm &) = delete;
    QVariantMap invoke(const char *operation, const QVariantMap &request, const Cancellation &cancel = {},
        const AudioReader &read = {}, QVariantMap *failureStatistics = nullptr, const AudioReaderInto &readInto = {}) const;
    QVariantMap description(const QVariantMap &parameters = {}, const QString &effort = {}) const;
    const QVariantMap &metadata() const { return m_metadata; }
    bool hasSession() const { return m_session != nullptr; }
    void runSession(const QVariantMap &input, const IntrovertDetectorServicesV1 &services) const;
private:
    mutable QLibrary m_library;
    const IntrovertDetectorApiV1 *m_api = nullptr;
    const IntrovertDetectorSessionApiV1 *m_session = nullptr;
    QVariantMap m_metadata;
};
} // namespace introvert::detection
