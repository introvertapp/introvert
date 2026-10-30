#include "contracts.h"
#include <QStringList>

namespace introvert::providers {

QString normalizedEpisodeOrder(QString value)
{
    value=value.toLower().remove(' ').remove('-').remove('_');
    const QMap<QString,QString> aliases{{"aired","official"},{"airedorder","official"},{"officialorder","official"},
        {"dvdorder","dvd"},{"altdvd","dvd"},{"absoluteorder","absolute"},{"abso","absolute"},
        {"alternateorder","alternate"},{"alttwo","alternate"},{"streaming","alternate"},{"streamingorder","alternate"},
        {"regionalorder","regional"},{"production","regional"},{"productionorder","regional"},{"defaultorder","default"}};
    value=aliases.value(value,value);
    return QStringList{"default","official","dvd","alternate","absolute","regional"}.contains(value)?value:QString();
}

QString providerName(ProviderId provider)
{
    switch (provider) {
    case ProviderId::Plex: return QStringLiteral("plex");
    case ProviderId::Emby: return QStringLiteral("emby");
    case ProviderId::Jellyfin: return QStringLiteral("jellyfin");
    case ProviderId::Local: return QStringLiteral("local");
    }
    throw ProviderError(QStringLiteral("Unsupported provider identifier"));
}

ProviderId providerId(const QString &name)
{
    for (const auto provider : {ProviderId::Plex, ProviderId::Emby, ProviderId::Jellyfin, ProviderId::Local})
        if (providerName(provider) == name) return provider;
    throw ProviderError(QStringLiteral("Unsupported provider identifier"));
}

QString segmentName(SegmentType kind)
{
    switch (kind) {
    case SegmentType::Recap: return QStringLiteral("recap");
    case SegmentType::Intro: return QStringLiteral("intro");
    case SegmentType::Credits: return QStringLiteral("credits");
    case SegmentType::Preview: return QStringLiteral("preview");
    }
    throw ProviderError(QStringLiteral("Unsupported imported segment type"));
}

SegmentType segmentType(const QString &name)
{
    for (const auto kind : {SegmentType::Recap, SegmentType::Intro, SegmentType::Credits, SegmentType::Preview})
        if (segmentName(kind) == name) return kind;
    throw ProviderError(QStringLiteral("Unsupported imported segment type"));
}

void checkCancelled(const Cancellation &cancelled)
{
    if (cancelled && cancelled->load(std::memory_order_relaxed)) throw Cancelled();
}

} // namespace introvert::providers
