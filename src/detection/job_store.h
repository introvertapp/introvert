#pragma once

#include "providers/contracts.h"
#include <QVariantList>

namespace introvert::detection {
class DetectionJobStore final {
public:
    explicit DetectionJobStore(QString databasePath) : m_path(std::move(databasePath)) {}
    QString databasePath() const { return m_path; }
    void initialize() const;
    QVariantList rows() const;
    QPair<QString, bool> create(const providers::ShowDetail &detail, int season, const QStringList &kinds) const;
    void progress(const QString &id, const QString &phase, int current, int total) const;
    void finish(const QString &id, const QString &status, const QString &message, const QVariantMap &result = {}, const QVariantMap &diagnostics = {}) const;
    QVariantList queue() const;
private:
    const QString m_path;
};
} // namespace introvert::detection
