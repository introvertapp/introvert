#pragma once

#include <QString>
#include <array>

namespace introvert {

struct EndpointDefinition {
    QString id, domain, name, enabledSetting;
};

inline const std::array<EndpointDefinition, 3> endpoints{{
    {"introdb_app", "introdb.app", "IntroDB", "introdb_enabled"},
    {"theintrodb_org", "theintrodb.org", "TheIntroDB", "theintrodb_enabled"},
    {"skipdb_tv", "skipdb.tv", "SkipDB", "skipdb_enabled"}
}};

inline const EndpointDefinition *endpointById(const QString &id)
{
    for (const auto &endpoint : endpoints)
        if (endpoint.id == id) return &endpoint;
    return nullptr;
}

inline QString endpointDomain(const QString &id)
{
    const auto *endpoint = endpointById(id);
    return endpoint ? endpoint->domain : id;
}

inline QString endpointName(const QString &domain)
{
    for (const auto &endpoint : endpoints)
        if (endpoint.domain == domain) return endpoint.name;
    return domain;
}

} // namespace introvert
