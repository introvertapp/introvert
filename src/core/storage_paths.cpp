#include "storage_paths.h"

#include <QDir>
#include <QStandardPaths>
#include <stdexcept>

namespace introvert {

StoragePaths StoragePaths::resolve(const QString &explicitRoot)
{
    // Match Path("") in the reference, without falling back to the user root.
    return fromRoot(explicitRoot.isEmpty() ? QStringLiteral(".") : explicitRoot);
}

StoragePaths StoragePaths::resolve()
{
    const auto base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (base.trimmed().isEmpty())
        throw std::runtime_error("OS did not provide an Introvert application data directory");
    return fromRoot(QDir(base).filePath(QStringLiteral("Introvert")));
}

StoragePaths StoragePaths::fromRoot(const QString &root)
{
    if (root.isEmpty()) throw std::runtime_error("Introvert application data directory is empty");
    return {root, QDir(root).filePath(QStringLiteral("data/app.db")),
            QDir(root).filePath(QStringLiteral("data/media-image-cache"))};
}

void StoragePaths::prepare() const
{
    if (!QDir().mkpath(QDir(root).filePath(QStringLiteral("data"))))
        throw std::runtime_error("Cannot create Introvert application data directory");
}

} // namespace introvert
