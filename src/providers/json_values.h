#pragma once

#include <QJsonValue>
#include <QUrl>
#include <QVariant>
#include <cmath>
#include <limits>
#include <optional>

namespace introvert::providers::detail {

inline bool present(const QJsonValue &value) { return !value.isNull() && !value.isUndefined(); }

inline QString sourceString(const QJsonValue &value)
{
    if (value.isString()) return value.toString();
    if (value.isBool()) return value.toBool() ? QStringLiteral("True") : QStringLiteral("False");
    if (!present(value)) return QStringLiteral("None");
    return value.toVariant().toString();
}

inline std::optional<QString> optionalString(const QJsonValue &value)
{
    return present(value) ? std::optional<QString>(sourceString(value)) : std::nullopt;
}

inline std::optional<double> finiteNumber(const QJsonValue &value, bool missingAsZero = false)
{
    double number;
    if (missingAsZero && (!present(value) || (value.isString() && value.toString().isEmpty()))) number = 0;
    else if (value.isDouble()) number = value.toDouble();
    else if (value.isBool()) number = value.toBool() ? 1 : 0;
    else if (value.isString()) {
        bool valid = false;
        number = value.toString().toDouble(&valid);
        if (!valid) return std::nullopt;
    } else return std::nullopt;
    return std::isfinite(number) ? std::optional<double>(number) : std::nullopt;
}

inline std::optional<qint64> nonnegativeNumber(const QJsonValue &value, bool positive = false)
{
    const auto number = finiteNumber(value);
    if (!number || (positive ? *number <= 0 : *number < 0)
        || *number >= double(std::numeric_limits<qint64>::max())) return std::nullopt;
    return static_cast<qint64>(*number);
}

inline QString quote(const QString &value) { return QString::fromLatin1(QUrl::toPercentEncoding(value)); }
inline QString baseUrl(QString value) { while (value.endsWith('/')) value.chop(1); return value; }

} // namespace introvert::providers::detail
