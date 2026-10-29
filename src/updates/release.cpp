#include "release.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QtGlobal>
#include <array>
#include <stdexcept>

namespace introvert::updates {
namespace {
std::array<int, 3> version(const QString &value)
{
    static const QRegularExpression pattern("^(0|[1-9][0-9]{0,4})\\.(0|[1-9][0-9]{0,4})\\.(0|[1-9][0-9]{0,4})$");
    const auto match = pattern.match(value);
    if (!match.hasMatch()) throw std::runtime_error("Release has an unsupported version.");
    const std::array<int, 3> parts{match.captured(1).toInt(), match.captured(2).toInt(), match.captured(3).toInt()};
    for (const auto part : parts) if (part > 65535) throw std::runtime_error("Release has an unsupported version.");
    return parts;
}
}

bool newerVersion(const QString &candidate, const QString &installed) { return version(candidate) > version(installed); }
QString platform()
{
#ifdef Q_OS_MACOS
    return "macos";
#elif defined(Q_OS_WIN)
    return "windows";
#else
    return "linux";
#endif
}
QString architecture()
{
#if defined(Q_PROCESSOR_ARM_64)
    return "arm64";
#elif defined(Q_PROCESSOR_X86_64)
    return "x86_64";
#else
    return {};
#endif
}
bool trustedDownload(const QUrl &url)
{
    return url.isValid() && url.scheme() == "https" && url.userInfo().isEmpty()
        && (url.port(-1) == -1 || url.port() == 443) && url.fragment().isEmpty()
        && (url.host() == "github.com" || url.host() == "release-assets.githubusercontent.com"
            || url.host() == "objects.githubusercontent.com");
}
Offer releaseOffer(const QJsonObject &release, const QString &installed, const QString &os,
                   const QString &arch, const QString &format)
{
    if (release["draft"].toBool(true) || release["prerelease"].toBool(true)) return {};
    const auto tag = release["tag_name"].toString();
    if (!tag.startsWith('v')) throw std::runtime_error("Release has an unsupported version.");
    const auto candidate = tag.mid(1);
    if (!newerVersion(candidate, installed)) return {};
    if ((arch != "arm64" && arch != "x86_64") || (os == "windows" && arch != "x86_64"))
        throw std::runtime_error("No application update is available for this architecture.");
    QString suffix;
    if (os == "macos") suffix = ".zip";
    else if (os == "windows") suffix = "-setup.exe";
    else if (os == "linux" && (format == "deb" || format == "rpm")) suffix = "." + format;
    else throw std::runtime_error("Cannot identify the installed DEB or RPM package. Use your system package manager.");
    const auto name = "Introvert-" + candidate + '-' + os + '-' + arch + suffix;
    Offer offer;
    for (const auto &entry : release["assets"].toArray()) {
        const auto asset = entry.toObject();
        if (asset["name"].toString() != name) continue;
        if (!offer.name.isEmpty()) throw std::runtime_error("Release contains duplicate application packages.");
        const auto digest = asset["digest"].toString();
        static const QRegularExpression hash("^sha256:[0-9a-f]{64}$");
        const QUrl url(asset["browser_download_url"].toString());
        const auto expectedPath = "/introvertapp/introvert/releases/download/" + tag + '/' + name;
        const auto bytes = asset["size"].toInteger();
        if (!hash.match(digest).hasMatch() || !trustedDownload(url) || url.host() != "github.com"
            || url.path() != expectedPath || !url.query().isEmpty() || bytes <= 0 || bytes > 1024LL * 1024 * 1024)
            throw std::runtime_error("Release package is missing verified download information.");
        offer = {candidate, name, url, digest.mid(7).toLatin1(), bytes};
    }
    if (offer.name.isEmpty()) throw std::runtime_error("The new release has no package for this platform.");
    return offer;
}
} // namespace introvert::updates

namespace introvert::updates {
Offer detectorReleaseOffer(const QJsonArray &releases, const QString &installed, const QString &app, const QString &os, const QString &arch, const QString &qt)
{
    Offer selected; QString incompatible;
    const QString begin = "<!-- introvert-detector-catalog-v1\n", end = "\n-->";
    for (const auto &value : releases) {
        const auto release = value.toObject(); const auto tag = release.value("tag_name").toString();
        if (!tag.startsWith("detector-v") || release.value("draft").toBool(true) || release.value("prerelease").toBool(true)) continue;
        const auto candidate = tag.mid(10); if (!newerVersion(candidate, installed)) continue;
        const auto body = release.value("body").toString(); const auto first = body.indexOf(begin), last = body.indexOf(end, first + begin.size());
        if (first < 0 || last < 0 || body.indexOf(begin, first + 1) >= 0) throw std::runtime_error("Detector release is missing its update catalogue.");
        const auto document = QJsonDocument::fromJson(body.mid(first + begin.size(), last - first - begin.size()).toUtf8());
        if (!document.isObject()) throw std::runtime_error("Detector release catalogue is invalid.");
        const auto catalog = document.object();
        if (catalog["schema_version"].toInt() != 1 || catalog["id"] != "introvert-detector" || catalog["version"] != candidate)
            throw std::runtime_error("Detector release catalogue identity does not match the release.");
        if (catalog["interface_version"].toInt() != 1 || catalog["result_schema_version"].toInt() != 1 || catalog["qt_version"] != qt
            || newerVersion(catalog["minimum_app_version"].toString(), app)) { incompatible = "The available detector requires a newer or compatible Introvert app."; continue; }
        Offer offer;
        const auto name = "Introvert-Detector-" + candidate + '-' + os + '-' + arch + ".tar";
        QJsonObject expected;
        for (const auto &artifact : catalog["artifacts"].toArray()) {
            const auto a = artifact.toObject(); if (a["platform"] != os || a["arch"] != arch) continue;
            if (!expected.isEmpty()) throw std::runtime_error("Detector catalogue has duplicate platform artifacts."); expected = a;
        }
        if (expected.isEmpty()) { incompatible = "The detector release has no package for this platform."; continue; }
        for (const auto &entry : release["assets"].toArray()) {
            const auto asset = entry.toObject(); if (asset["name"] != name) continue;
            if (!offer.name.isEmpty()) throw std::runtime_error("Detector release contains duplicate packages.");
            const auto hash = asset["digest"].toString(); const QUrl url(asset["browser_download_url"].toString()); const auto bytes = asset["size"].toInteger();
            if (!QRegularExpression("^sha256:[0-9a-f]{64}$").match(hash).hasMatch() || expected["name"] != name || expected["sha256"] != hash.mid(7)
                || expected["bytes"].toInteger() != bytes || bytes <= 0 || bytes > 256LL * 1024 * 1024
                || !trustedDownload(url) || url.host() != "github.com" || !url.query().isEmpty()
                || url.path() != "/introvertapp/introvert/releases/download/" + tag + '/' + name)
                throw std::runtime_error("Detector package is missing verified download information.");
            offer = {candidate, name, url, hash.mid(7).toLatin1(), bytes};
        }
        if (offer.name.isEmpty()) throw std::runtime_error("Detector catalogue refers to an unpublished package.");
        if (selected.name.isEmpty() || newerVersion(offer.version, selected.version)) selected = offer;
    }
    if (selected.name.isEmpty() && !incompatible.isEmpty()) throw std::runtime_error(incompatible.toStdString());
    return selected;
}
} // namespace introvert::updates
