#include "algorithm_package.h"
#include "algorithm_client.h"
#include "core/sqlite.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QRegularExpression>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QVersionNumber>
#include <algorithm>
#include <optional>

namespace introvert::detection {
namespace {
[[noreturn]] void invalid(const char *message) { throw DetectionError("algorithm_package_invalid", QString::fromLatin1(message)); }
QString digest(const QByteArray &bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); }
QByteArray read(const QString &path, qint64 maximum)
{
    QFile file(path); if (!file.open(QIODevice::ReadOnly) || file.size() < 0 || file.size() > maximum) invalid("Algorithm package file is missing or oversized.");
    const auto bytes = file.readAll(); if (bytes.size() != file.size()) invalid("Cannot read the complete algorithm package file."); return bytes;
}
bool safe(const QString &path)
{
    static const QRegularExpression chars("^[A-Za-z0-9_.-]+(?:/[A-Za-z0-9_.-]+)*$");
    static const QRegularExpression reserved("^(?:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\.|$)", QRegularExpression::CaseInsensitiveOption);
    if (path.size() > 200 || !chars.match(path).hasMatch()) return false;
    for (const auto &part : path.split('/')) if (part == "." || part == ".." || part.endsWith('.') || reserved.match(part).hasMatch()) return false;
    return true;
}
QVersionNumber version(const QString &text)
{
    if (!QRegularExpression("^(0|[1-9][0-9]{0,4})\\.(0|[1-9][0-9]{0,4})\\.(0|[1-9][0-9]{0,4})$").match(text).hasMatch()) invalid("Invalid algorithm release version.");
    return QVersionNumber::fromString(text);
}
qint64 octal(const QByteArray &bytes)
{
    auto value = bytes; const auto zero = value.indexOf('\0'); if (zero >= 0) value.truncate(zero); value = value.trimmed();
    if (value.isEmpty() || std::any_of(value.begin(), value.end(), [](char c) { return c < '0' || c > '7'; })) invalid("Invalid algorithm archive number.");
    bool ok; const auto result = value.toLongLong(&ok, 8); if (!ok || result < 0) invalid("Invalid algorithm archive number."); return result;
}
void expand(const QString &archive, const QString &destination, const std::function<void()> &cancel)
{
    QFile input(archive); if (!input.open(QIODevice::ReadOnly) || input.size() > 256 * 1024 * 1024 || input.size() < 1024) invalid("Invalid algorithm archive size.");
    QSet<QString> names; qint64 expanded = 0; int zeroBlocks = 0;
    while (!input.atEnd()) {
        if (cancel) cancel(); const auto header = input.read(512); if (header.size() != 512) invalid("Truncated algorithm archive.");
        if (header == QByteArray(512, '\0')) { if (++zeroBlocks == 2) { if (input.readAll().trimmed().replace('\0', "").size()) invalid("Unexpected algorithm archive trailer."); return; } continue; }
        if (zeroBlocks) invalid("Invalid algorithm archive trailer.");
        qint64 checksum = 0; for (int i = 0; i < 512; ++i) checksum += i >= 148 && i < 156 ? ' ' : static_cast<unsigned char>(header[i]);
        if (checksum != octal(header.mid(148, 8)) || header.mid(257, 5) != "ustar") invalid("Invalid algorithm archive header.");
        if (header[345] != '\0' || header[157] != '\0') invalid("Unsupported algorithm archive path or link.");
        auto rawName = header.first(100); const auto end = rawName.indexOf('\0'); if (end >= 0) rawName.truncate(end);
        const auto name = QString::fromUtf8(rawName); if (name.toUtf8() != rawName || !name.startsWith("detector/")) invalid("Unsafe algorithm archive path.");
        auto relative = name.mid(9); if (relative.endsWith('/')) relative.chop(1);
        const auto size = octal(header.mid(124, 12)); const char type = header[156];
        if (type == '5') { if (size || (!relative.isEmpty() && !safe(relative))) invalid("Invalid algorithm archive directory."); continue; }
        if ((type != '0' && type != '\0') || !safe(relative) || names.contains(relative.toCaseFolded()) || names.size() >= 128
            || size > 64 * 1024 * 1024 || expanded + size > 256 * 1024 * 1024 || input.bytesAvailable() < (size + 511) / 512 * 512)
            invalid("Unsafe or oversized algorithm archive entry.");
        names.insert(relative.toCaseFolded()); expanded += size;
        const auto target = QDir(destination).filePath(relative); if (!QDir().mkpath(QFileInfo(target).absolutePath())) invalid("Cannot stage algorithm files.");
        QFile output(target); if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly)) invalid("Cannot stage algorithm file.");
        qint64 remaining = size;
        while (remaining) { if (cancel) cancel(); const auto part = input.read(std::min(remaining, qint64(65536))); if (part.isEmpty() || output.write(part) != part.size()) invalid("Incomplete algorithm archive entry."); remaining -= part.size(); }
        output.close(); input.seek(input.pos() + (512 - size % 512) % 512);
    }
    invalid("Algorithm archive has no complete trailer.");
}
}
QString algorithmPlatform()
{
#ifdef Q_OS_MACOS
    return "macos";
#elif defined(Q_OS_WIN)
    return "windows";
#else
    return "linux";
#endif
}
QString algorithmArchitecture()
{
    const auto cpu = QSysInfo::buildCpuArchitecture(); return cpu == "aarch64" || cpu == "arm64" ? "arm64" : cpu == "x86_64" ? "x86_64" : cpu;
}
QString bundledAlgorithmRoot()
{
    const auto base = QCoreApplication::applicationDirPath();
#ifdef Q_OS_MACOS
    const auto packaged = QDir::cleanPath(base + "/../Resources/detector"); if (QFileInfo(packaged).isDir()) return packaged;
#endif
    return base + "/detector";
}
AlgorithmPackage inspectAlgorithmPackage(const QString &root)
{
    if (!QFileInfo(root).isDir() || QFileInfo(root).isSymLink()) invalid("Invalid algorithm package directory.");
    const auto bytes = read(QDir(root).filePath("detector.json"), 1024 * 1024); const auto document = QJsonDocument::fromJson(bytes);
    if (!document.isObject()) invalid("Invalid algorithm manifest."); const auto m = document.object().toVariantMap();
    if (m.value("id") != "introvert-detector" || m.value("interface_version").toInt() != algorithmInterfaceVersion
        || m.value("result_schema_version").toInt() != algorithmResultSchemaVersion || m.value("qt_version") != QT_VERSION_STR
        || m.value("platform") != algorithmPlatform() || m.value("arch") != algorithmArchitecture() || m.value("window_policy") != "hard-audio-windows-v1")
        invalid("The detector package is incompatible with this app or platform.");
    version(m.value("version").toString());
    const auto appVersion = QCoreApplication::applicationVersion().isEmpty() ? QStringLiteral(INTROVERT_VERSION) : QCoreApplication::applicationVersion();
    if (version(m.value("minimum_app_version").toString()) > version(appVersion)) invalid("The detector package requires a newer INTROvert app.");
    const auto library = m.value("library").toString(); const auto files = m.value("files").toMap();
    if (!safe(library) || !files.contains(library) || files.isEmpty() || files.size() > 127 || files.contains("detector.json")) invalid("Invalid algorithm package inventory.");
    QSet<QString> seen; qint64 total = 0;
    for (auto it = files.begin(); it != files.end(); ++it) {
        const auto item = it.value().toMap(); const auto path = QDir(root).filePath(it.key()); const QFileInfo info(path);
        if (!safe(it.key()) || seen.contains(it.key().toCaseFolded()) || !info.isFile() || info.isSymLink() || info.size() != item.value("bytes").toLongLong()
            || info.size() > 64 * 1024 * 1024 || (total += info.size()) > 256 * 1024 * 1024 || digest(read(path, 64 * 1024 * 1024)) != item.value("sha256"))
            invalid("Algorithm package integrity verification failed.");
        seen.insert(it.key().toCaseFolded());
    }
    QDirIterator items(root, QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (items.hasNext()) { items.next(); const auto info = items.fileInfo(); const auto relative = QDir(root).relativeFilePath(info.filePath());
        if (info.isSymLink() || (!info.isDir() && (!info.isFile() || (relative != "detector.json" && !files.contains(relative))))) invalid("Unexpected algorithm package content."); }
    return {QDir(root).absolutePath(), QDir(root).absoluteFilePath(library), m.value("version").toString(), digest(bytes), m};
}
AlgorithmPackages::AlgorithmPackages(QString database, QString bundled) : m_database(std::move(database)), m_bundled(bundled.isEmpty() ? bundledAlgorithmRoot() : std::move(bundled)) {}
QString AlgorithmPackages::root() const { return QFileInfo(m_database).absolutePath() + "/detection-algorithms"; }
AlgorithmPackage AlgorithmPackages::selected() const
{
    std::optional<AlgorithmPackage> bundled;
    try { bundled=inspectAlgorithmPackage(m_bundled); } catch (...) {}
    sql::Connection connection(m_database);
    auto exists = sql::execute(connection.db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name='detection_algorithm_state'");
    if (exists.next()) {
        auto row = sql::execute(connection.db, "SELECT current_path,current_hash,previous_path,previous_hash FROM detection_algorithm_state WHERE id=1");
        if (row.next()) for (const int column : {0, 2}) {
            const auto directory = row.value(column).toString(); if (!safe(directory) || directory.contains('/')) continue;
            try {
                const auto package = inspectAlgorithmPackage(QDir(root()).filePath(directory));
                if (package.manifestHash == row.value(column + 1).toString())
                    return bundled && version(bundled->version)>version(package.version) ? *bundled : package;
            } catch (...) {}
        }
    }
    if (bundled) return *bundled;
    invalid("No compatible intact detector package is available.");
}
AlgorithmPackage AlgorithmPackages::install(const QString &archive, const QString &expectedVersion, const std::function<void()> &cancel) const
{
    version(expectedVersion); if (!QDir().mkpath(root())) invalid("Cannot create algorithm installation storage.");
    QLockFile lock(root() + "/.install.lock"); lock.setStaleLockTime(0); if (!lock.tryLock()) invalid("Another detector update is being installed.");
    QTemporaryDir staging(root() + "/.stage-XXXXXX"); if (!staging.isValid()) invalid("Cannot create algorithm staging storage.");
    expand(archive, staging.path(), cancel); auto candidate = inspectAlgorithmPackage(staging.path());
    const auto prior = selected();
    if (version(candidate.version) < version(prior.version)) invalid("Refusing an automatic detector downgrade.");
    if (candidate.version == prior.version && candidate.manifestHash != prior.manifestHash) invalid("A detector version cannot be replaced with different package contents.");
    if (candidate.version != expectedVersion) invalid("Algorithm package version does not match its release offer.");
    { Algorithm probe(candidate); probe.description(); } // Validate code/runtime before making it selectable.
    if (cancel) cancel(); const auto originalHash = candidate.manifestHash; const auto directory = candidate.version + '-' + originalHash;
    const auto destination = QDir(root()).filePath(directory);
    if (QFileInfo::exists(destination)) { const auto existing = inspectAlgorithmPackage(destination); if (existing.manifestHash != candidate.manifestHash) invalid("Immutable algorithm installation changed."); }
    else if (!QDir().rename(staging.path(), destination)) invalid("Cannot promote algorithm package.");
    candidate = inspectAlgorithmPackage(destination);
    if (candidate.manifestHash != originalHash || candidate.version != expectedVersion) invalid("Algorithm package changed during promotion.");
    { Algorithm probe(candidate); probe.description(); }
    if (inspectAlgorithmPackage(destination).manifestHash != originalHash) invalid("Algorithm package changed during validation.");
    if (cancel) cancel();
    sql::Connection connection(m_database); sql::Transaction transaction(connection.db);
    sql::execute(connection.db, "CREATE TABLE IF NOT EXISTS detection_algorithm_state(id INTEGER PRIMARY KEY CHECK(id=1),current_path TEXT NOT NULL,current_hash TEXT NOT NULL,previous_path TEXT NOT NULL,previous_hash TEXT NOT NULL)");
    auto current = sql::execute(connection.db, "SELECT current_path,current_hash FROM detection_algorithm_state WHERE id=1");
    const bool unchanged = current.next() && current.value(0).toString() == directory && current.value(1).toString() == candidate.manifestHash; current.finish();
    if (!unchanged) {
        // Retain the package actually selected, which may already be a fallback
        // from a damaged current package. Never replace a working fallback with
        // a pointer to the damaged one.
        QString oldPath = QStringLiteral(""), oldHash = QStringLiteral("");
        if (QFileInfo(prior.root).absolutePath() == QDir(root()).absolutePath() && QFileInfo(prior.root).fileName() != directory) {
            oldPath = QFileInfo(prior.root).fileName(); oldHash = prior.manifestHash;
        }
        sql::execute(connection.db, "INSERT OR REPLACE INTO detection_algorithm_state VALUES(1,?,?,?,?)", {directory, candidate.manifestHash, oldPath, oldHash});
    }
    if (cancel) cancel(); transaction.commit(); return candidate;
}
} // namespace introvert::detection
