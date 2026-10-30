#pragma once

#include <QJsonObject>
#include <QJsonArray>
#include <QString>
#include <QUrl>

namespace introvert::updates {

struct Offer {
    QString version, name;
    QUrl download;
    QByteArray sha256;
    qint64 bytes = 0;
};

// A stable release and its exact architecture/package are selected together.
// Source archives, manifests, prereleases and downgrades are never installable.
bool newerVersion(const QString &candidate, const QString &installed);
QString platform();
QString architecture();
bool trustedDownload(const QUrl &url);
QString latestApplicationVersion(const QJsonObject &release);
QString latestDetectorVersion(const QJsonArray &releases, const QString &appVersion,
    const QString &platform, const QString &arch, const QString &qtVersion);
Offer releaseOffer(const QJsonObject &release, const QString &installed,
                   const QString &platform, const QString &arch, const QString &packageFormat = {});

Offer detectorReleaseOffer(const QJsonArray &releases, const QString &installed, const QString &appVersion,
    const QString &platform, const QString &arch, const QString &qtVersion);

} // namespace introvert::updates
