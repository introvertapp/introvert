#pragma once

#include "core/settings_store.h"
#include "providers/contracts.h"
#include <QUrl>
#include <functional>

namespace introvert::notifications {

class WebhookError final : public std::invalid_argument {
public:
    explicit WebhookError(const QString &message) : std::invalid_argument(message.toStdString()) {}
};

struct WebhookRequest {
    QUrl url;
    QMap<QByteArray, QByteArray> headers;
    QByteArray body;
};

QVariantMap testNotification(std::optional<double> now = {});
// Input is the committed detection_jobs row, including its JSON result column.
// Detection orchestration calls delivery only after a successful commit.
QVariantMap completionNotification(const QVariantMap &job);
WebhookRequest prepareWebhook(const QString &url, const QString &key, const QVariantMap &notification);
void sendWebhook(const QString &url, const QString &key, const QVariantMap &notification,
                 const providers::Cancellation &cancelled = {});

class WebhookService final {
public:
    using Sender = std::function<void(const QString &, const QString &, const QVariantMap &, const providers::Cancellation &)>;
    explicit WebhookService(SettingsStore settings, Sender sender = {});
    // A missing draft uses the saved key; an explicitly empty draft omits it.
    // Testing does not save values and does not require the enabled toggle.
    void test(const QVariantMap &values, const std::optional<QString> &key,
              const providers::Cancellation &cancelled = {}) const;
    QVariantMap deliverCompletion(const QVariantMap &job, const providers::Cancellation &cancelled = {}) const;
private:
    SettingsStore m_settings;
    Sender m_sender;
};

} // namespace introvert::notifications
