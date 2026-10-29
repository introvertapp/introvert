#pragma once
#include "contracts.h"
#include <QElapsedTimer>
#include <QUrl>

namespace introvert::providers {
// One grace period per new remote origin in this process. Read-only requests
// may wait for a privacy/firewall prompt; submissions are never replayed.
class AccessRetry final {
public:
    explicit AccessRetry(const QUrl &url, bool enabled = defaultEnabled(), int budgetMs = 60000);
    bool pending() const;
    int remainingMs() const;
    void complete();
    void wait(const Cancellation &cancelled) const;
    static bool defaultEnabled();
private:
    QString m_origin;
    QElapsedTimer m_clock;
    bool m_enabled;
    int m_budgetMs;
};
bool readableMediaFile(const QString &path, const Cancellation &cancelled);
bool readableMediaDirectory(const QString &path, const Cancellation &cancelled);
}
