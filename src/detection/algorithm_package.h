#pragma once
#include <QVariantMap>
#include <QString>
#include <functional>

namespace introvert::detection {
QString algorithmPlatform();
QString algorithmArchitecture();
QString bundledAlgorithmRoot();
struct AlgorithmPackage { QString root, library, version, manifestHash; QVariantMap manifest; };
AlgorithmPackage inspectAlgorithmPackage(const QString &root);
class AlgorithmPackages final {
public:
    explicit AlgorithmPackages(QString database, QString bundled = {});
    AlgorithmPackage selected() const;
    AlgorithmPackage install(const QString &archive, const QString &expectedVersion, const std::function<void()> &cancel = {}) const;
    QString root() const;
private:
    QString m_database, m_bundled;
};
} // namespace introvert::detection
