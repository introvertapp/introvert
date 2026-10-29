#pragma once

#include <QString>

namespace introvert {

struct StoragePaths {
    QString root;
    QString database;
    QString artwork;

    // An explicit override is used verbatim. Discovery does not create files.
    static StoragePaths resolve();
    static StoragePaths resolve(const QString &explicitRoot);
    static StoragePaths fromRoot(const QString &root);
    void prepare() const;
};

} // namespace introvert
