#include "webhook_service.h"
#include "providers/http_transport.h"
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTimeZone>
#include <algorithm>
#include <charconv>
#include <cmath>

namespace introvert::notifications {
namespace {
QString iso(double seconds)
{
    if (!std::isfinite(seconds) || seconds < -62135596800. || seconds >= 253402300800.)
        throw WebhookError("Cannot format notification timestamp.");
    const auto milliseconds = std::floor(std::nearbyint(seconds * 1000000) / 1000);
    return QDateTime::fromMSecsSinceEpoch(qint64(milliseconds), QTimeZone::UTC).toString(Qt::ISODateWithMs);
}
void finiteNumbers(const QVariant &value)
{
    if (value.metaType().id() == QMetaType::Double && !std::isfinite(value.toDouble()))
        throw WebhookError("Cannot format webhook notification.");
    if (value.metaType().id() == QMetaType::QVariantMap)
        for (const auto &item : value.toMap()) finiteNumbers(item);
    else if (value.metaType().id() == QMetaType::QVariantList)
        for (const auto &item : value.toList()) finiteNumbers(item);
}
}

QVariantMap testNotification(std::optional<double> now)
{
    return {{"event", "notification.test"}, {"test", true}, {"title", "Introvert notification test"},
        {"message", QString::fromUtf8("Seinfeld • S01 • 24 segments • 4.5 min")}, {"show", QVariantMap{{"title", "Seinfeld"}}},
        {"seasons", QVariantList{"S01"}}, {"segmentCount", 24}, {"duration", "4.5 min"}, {"durationMs", 270000},
        {"sentAt", iso(now.value_or(QDateTime::currentMSecsSinceEpoch() / 1000.))}};
}

QVariantMap completionNotification(const QVariantMap &job)
{
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(job.value("result").toString().toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) throw WebhookError("Cannot format detection notification.");
    const auto result = document.object().toVariantMap();
    const auto kinds = result.value("segmentTypes", QVariantList{}).toList();
    int count = 0;
    for (const auto &row : result.value("updated").toMap()) for (const auto &kind : kinds) {
        const auto value = row.toMap().value(kind.toString());
        if (value.metaType().id() != QMetaType::QVariantList) continue;
        const auto pair = value.toList();
        if (pair.size() == 2 && !pair[0].isNull() && !pair[1].isNull()) ++count;
    }
    const auto durationMs = std::max(0., result.value("durationMs", 0).toDouble());
    // Use fixed precision/ties-to-even formatting, independently of
    // the process locale. QString::number rounds exact .25 to .3 instead of .2.
    char buffer[512];
    const auto formatted = std::to_chars(std::begin(buffer), std::end(buffer), durationMs / 60000, std::chars_format::fixed, 1);
    if (formatted.ec != std::errc()) throw WebhookError("Cannot format detection notification.");
    const auto duration = QString::fromLatin1(buffer, formatted.ptr - buffer) + " min";
    const auto season = QString("S%1").arg(std::max(0, job.value("season").toInt()), 2, 10, QChar('0'));
    const auto title = job.value("title").toString().isEmpty() ? QString("TV show") : job.value("title").toString();
    return {{"event", "segment_detection.completed"}, {"title", "Segment detection complete"},
        {"message", QString::fromUtf8("%1 • %2 • %3 %4 • %5").arg(title, season, QString::number(count), count == 1 ? "segment" : "segments", duration)},
        {"jobId", job.value("id")}, {"show", QVariantMap{{"id", job.value("show_id")}, {"title", title}}},
        {"seasons", QVariantList{season}}, {"segmentCount", count}, {"duration", duration}, {"durationMs", durationMs},
        {"segmentTypes", kinds}, {"completedAt", iso(job.value("updated").toDouble())}};
}

WebhookRequest prepareWebhook(const QString &value, const QString &secret, const QVariantMap &notification)
{
    const auto url = value.trimmed(), key = secret.trimmed();
    if (!SettingsStore::validWebhookUrl(url)) throw WebhookError("Enter a valid HTTP or HTTPS webhook URL.");
    if (key.contains('\r') || key.contains('\n')) throw WebhookError("Webhook key must not contain line breaks.");
    WebhookRequest request; request.url = QUrl(url, QUrl::StrictMode);
    const auto host = request.url.host().toLower();
    const bool discord = (host == "discord.com" || host == "discordapp.com")
        && QRegularExpression("^/api(?:/v[0-9]+)?/webhooks/").match(request.url.path(QUrl::FullyEncoded)).hasMatch();
    const auto payload = discord ? QVariantMap{{"content", "**" + notification.value("title").toString() + "**\n" + notification.value("message").toString()}} : notification;
    finiteNumbers(payload);
    request.body = QJsonDocument::fromVariant(payload).toJson(QJsonDocument::Compact);
    request.headers = {{"Content-Type", "application/json"}, {"Accept", "application/json"}, {"User-Agent", "Introvert/" INTROVERT_VERSION}};
    if (!key.isEmpty() && !discord) {
        request.headers["Authorization"] = "Bearer " + key.toUtf8(); request.headers["X-Introvert-Webhook-Key"] = key.toUtf8();
    }
    return request;
}

void sendWebhook(const QString &url, const QString &key, const QVariantMap &notification, const providers::Cancellation &cancelled)
{
    const auto request = prepareWebhook(url, key, notification);
    int status;
    try {
        status = providers::detail::exchange(request.url, request.headers, 10, 0, cancelled, request.body, true, true).status;
    } catch (const providers::Cancelled &) { throw; }
    catch (...) { throw WebhookError("Webhook request failed. Check the URL, network and server certificate."); }
    if (status < 200 || status >= 300) throw WebhookError(QString("Webhook returned HTTP %1").arg(status));
}

WebhookService::WebhookService(SettingsStore settings, Sender sender) : m_settings(std::move(settings)), m_sender(std::move(sender))
{
    if (!m_sender) m_sender = sendWebhook;
}

void WebhookService::test(const QVariantMap &values, const std::optional<QString> &secret, const providers::Cancellation &cancelled) const
{
    const auto key = secret ? *secret : m_settings.credential("webhook:key").value_or(QString());
    m_sender(values.value("url").toString(), key, testNotification(), cancelled);
}

QVariantMap WebhookService::deliverCompletion(const QVariantMap &job, const providers::Cancellation &cancelled) const
{
    try {
        const auto values = m_settings.loadWebhook();
        if (!values.value("enabled").toBool()) return {{"attempted", false}};
        const auto key = m_settings.credential("webhook:key").value_or(QString());
        m_sender(values.value("url").toString(), key, completionNotification(job), cancelled);
        return {{"attempted", true}, {"success", true}};
    } catch (...) {
        return {{"attempted", true}, {"success", false}, {"error", "Webhook notification could not be delivered."}};
    }
}

} // namespace introvert::notifications
