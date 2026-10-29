#pragma once
#include "algorithm_api.h"
#include "algorithm_data.h"
#include "algorithm_package.h"
#include <QLibrary>

namespace introvert::detection {
// Job-owned library handle. Updating the selected package cannot change a
// running job, its code, rules or dependencies.
class Algorithm final {
public:
    using AudioReader = std::function<std::vector<qint16>(uint32_t source, uint32_t rate, qint64 first, uint32_t count)>;
    explicit Algorithm(const AlgorithmPackage &package);
    ~Algorithm();
    Algorithm(const Algorithm &) = delete;
    Algorithm &operator=(const Algorithm &) = delete;
    QVariantMap invoke(const char *operation, const QVariantMap &request, const Cancellation &cancel = {},
        const AudioReader &read = {}, QVariantMap *failureStatistics = nullptr) const;
    QVariantMap description(const QVariantMap &parameters = {}, const QString &effort = {}) const;
    const QVariantMap &metadata() const { return m_metadata; }
private:
    mutable QLibrary m_library;
    const IntrovertDetectorApiV1 *m_api = nullptr;
    QVariantMap m_metadata;
};
} // namespace introvert::detection
