#pragma once

#include "contracts.h"

#include <QByteArray>
#include <QJsonValue>
#include <QMap>
#include <QVariantMap>

namespace introvert::providers {

class HttpClient final {
public:
    static QJsonValue json(const ProviderConfig &config, const QString &path, const QVariantMap &parameters,
                           const QString &label, const QMap<QByteArray, QByteArray> &headers,
                           const Cancellation &cancelled = {});
    static QByteArray image(const ProviderConfig &config, const QString &path, const QString &label,
                            const QMap<QByteArray, QByteArray> &headers, const Cancellation &cancelled = {});
private:
    static QByteArray read(const ProviderConfig &config, const QString &path, const QVariantMap &parameters,
                           const QString &label, const QMap<QByteArray, QByteArray> &headers,
                           bool image, const Cancellation &cancelled);
};

} // namespace introvert::providers
