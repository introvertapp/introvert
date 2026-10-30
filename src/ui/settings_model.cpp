#include "settings_model.h"

#include <QMetaObject>
#include <QRunnable>
#include <QUrl>
#include <cmath>
#include <functional>
#include <utility>

namespace introvert::ui {
namespace {

QString errorText(const std::exception &error) { return QString::fromUtf8(error.what()); }

struct OperationResult {
    QVariantMap values;
    QVariantMap status;
    QString error;
};

void startOperation(QThreadPool &pool, QObject *owner, std::function<OperationResult()> work,
                    std::function<void(const OperationResult &)> finished)
{
    pool.start(QRunnable::create([owner, work = std::move(work), finished = std::move(finished)] {
        OperationResult result;
        try { result = work(); }
        catch (const SettingsError &error) { result.error = errorText(error); }
        catch (...) { result.error = QStringLiteral("Cannot access INTROvert settings or API keys in app.db."); }
        // The owner's destructor joins its pool before QObject destruction.
        // Qt drops an undelivered callback when its context is destroyed.
        QMetaObject::invokeMethod(owner, [finished, result] { finished(result); }, Qt::QueuedConnection);
    }));
}

QVariantMap endpointKeyStatus(const SettingsStore &store)
{
    return {{"introdb_app", store.hasCredential("endpoints:introdb_app")},
            {"theintrodb_org", store.hasCredential("endpoints:theintrodb_org")}};
}

QVariant endpointInput(const QString &name, const QVariant &value)
{
    // A JS number arriving through QVariant is double even when the unchanged
    // SpinBox supplies a whole number. Normalize only that Qt boundary case.
    if ((name == "api_timeout_ms" || name == "api_delay_seconds")
        && value.metaType().id() == QMetaType::Double) {
        const auto number = value.toDouble();
        if (std::isfinite(number) && std::trunc(number) == number && number >= -2147483647 && number <= 2147483647)
            return static_cast<int>(number);
    }
    return value;
}

} // namespace

ProviderSettingsModel::ProviderSettingsModel(SettingsStore store, const QString &provider, QObject *parent)
    : QObject(parent), m_store(std::move(store))
{
    m_timer.setSingleShot(true);
    m_timer.setInterval(450);
    connect(&m_timer, &QTimer::timeout, this, &ProviderSettingsModel::flush);
    select(provider);
}

ProviderSettingsModel::~ProviderSettingsModel() { flush(); }

bool ProviderSettingsModel::dirty() const
{
    return m_edited || m_address != m_savedAddress || m_images != m_savedImages;
}

void ProviderSettingsModel::refreshStatus()
{
    if (m_provider.isEmpty() || m_provider == "local") { m_status = QStringLiteral("Not applicable"); return; }
    try { m_status = m_store.hasCredential("media:" + m_provider) ? QStringLiteral("Saved") : QStringLiteral("Not saved"); }
    catch (...) { m_status = QStringLiteral("Unavailable — check app.db access"); }
}

void ProviderSettingsModel::select(const QString &provider)
{
    if (provider != m_provider && dirty()) flush();
    if (provider == m_provider && dirty()) return;
    if (!m_provider.isEmpty() && dirty()) m_drafts[m_provider] = {m_address, m_images, m_credential, m_edited};
    m_timer.stop();
    m_provider = provider;
    m_address.clear(); m_credential.clear(); m_message.clear();
    m_images = m_edited = false;
    m_timeout = 10;
    if (!provider.isEmpty()) {
        try {
            const auto config = m_store.loadProvider(provider);
            m_address = provider == "local" ? config.localRoot : config.remoteUrl;
            m_images = config.fetchImages;
            m_timeout = config.timeoutSeconds;
            if (provider != "local") m_credential = m_store.credential("media:" + provider).value_or(QString());
        } catch (...) {
            m_message = QStringLiteral("Cannot load provider settings. Check database access and app.db credential storage.");
        }
    }
    refreshStatus();
    m_savedAddress = m_address; m_savedImages = m_images;
    const auto draft = m_drafts.constFind(provider);
    if (draft != m_drafts.cend()) {
        m_address = draft->address; m_images = draft->images;
        m_credential = draft->credential; m_edited = draft->edited;
    }
    emit changed();
    emit cleared();
}

void ProviderSettingsModel::edited()
{
    m_message.clear(); m_timer.start(); emit changed();
}

void ProviderSettingsModel::setAddress(const QString &value) { m_address = value; edited(); }
void ProviderSettingsModel::setFetchImages(bool value) { m_images = value; edited(); }
void ProviderSettingsModel::setCredentialInput(const QString &value)
{
    m_credential = value; m_edited = true; edited();
}

void ProviderSettingsModel::setDirectoryUrl(const QString &value)
{
    const auto path = QUrl(value).toLocalFile();
    if (m_provider != "local" || path.isEmpty()) {
        m_message = QStringLiteral("Choose a local media directory."); emit changed(); return;
    }
    setAddress(path);
}

void ProviderSettingsModel::flush()
{
    m_timer.stop();
    if (!m_provider.isEmpty() && dirty())
        save(!m_edited ? CredentialAction::Keep : m_credential.isEmpty() ? CredentialAction::Delete : CredentialAction::Replace,
             m_edited ? m_credential : QString());
}

bool ProviderSettingsModel::saveCredentialDraft(bool edited, const QString &credential)
{
    return save(!edited ? CredentialAction::Keep : credential.isEmpty() ? CredentialAction::Delete : CredentialAction::Replace,
                edited ? credential : QString());
}

bool ProviderSettingsModel::save(CredentialAction action, const QString &credential)
{
    if (m_provider.isEmpty()) { m_message = QStringLiteral("Select a provider first."); emit changed(); return false; }
    try {
        m_store.saveProvider({m_provider, m_provider == "local" ? QString() : m_address,
                             m_provider == "local" ? m_address : QString(), m_images, m_timeout}, action, credential);
    } catch (const SettingsError &error) { m_message = errorText(error); emit errorOccurred(m_message); emit changed(); return false; }
    catch (...) { m_message = QStringLiteral("Save failed. Check app.db access and retry."); emit errorOccurred(m_message); emit changed(); return false; }
    m_edited = false;
    m_drafts.remove(m_provider);
    m_savedAddress = m_address; m_savedImages = m_images;
    m_message.clear(); refreshStatus(); emit changed(); emit saved();
    return true;
}

EndpointSettingsModel::EndpointSettingsModel(SettingsStore store, QObject *parent)
    : QObject(parent), m_store(std::move(store)), m_values(m_store.loadEndpoints())
{
    m_pool.setMaxThreadCount(1);
    m_timer.setSingleShot(true); m_timer.setInterval(400);
    connect(&m_timer, &QTimer::timeout, this, &EndpointSettingsModel::save);
}

EndpointSettingsModel::~EndpointSettingsModel() { close(); }
void EndpointSettingsModel::edited() { ++m_revision; m_message.clear(); m_timer.start(); emit changed(); }
void EndpointSettingsModel::setValue(const QString &name, const QVariant &value)
{
    if (m_closed || !m_values.contains(name)) return;
    m_values[name] = endpointInput(name, value); edited();
}
void EndpointSettingsModel::editKey(const QString &name, const QString &value)
{
    if (m_closed || (name != "introdb_app" && name != "theintrodb_org")) return;
    m_keys[name] = value; edited();
}

void EndpointSettingsModel::activate()
{
    if (m_closed || m_busy) return;
    m_busy = true;
    startOperation(m_pool, this, [store = m_store] { return OperationResult{{}, endpointKeyStatus(store), {}}; },
        [this](const OperationResult &result) {
            if (m_closed) return;
            m_busy = false; m_message = result.error;
            if (!result.error.isEmpty()) emit errorOccurred(result.error);
            if (result.error.isEmpty()) m_keyStatus = result.status;
            if (m_revision > m_persistedRevision && result.error.isEmpty()) m_timer.start();
            emit changed();
        });
    emit changed();
}

void EndpointSettingsModel::save()
{
    if (m_closed) return;
    if (m_busy) { m_timer.start(); return; }
    m_timer.stop();
    try { SettingsStore::validateEndpoints(m_values); }
    catch (const SettingsError &error) { m_message = errorText(error); emit errorOccurred(m_message); emit changed(); return; }
    const auto revision = m_revision;
    m_busy = true;
    startOperation(m_pool, this, [store = m_store, values = m_values, keys = m_keys] {
        store.saveEndpoints(values, keys);
        return OperationResult{values, endpointKeyStatus(store), {}};
    }, [this, revision](const OperationResult &result) {
        if (m_closed) return;
        m_busy = false; m_message = result.error;
        if (!result.error.isEmpty()) emit errorOccurred(result.error);
        if (result.error.isEmpty()) {
            m_persistedRevision = revision; m_keyStatus = result.status;
            if (m_revision == revision) { m_values = result.values; m_keys.clear(); }
            emit saved();
        }
        if (m_revision > m_persistedRevision && result.error.isEmpty()) m_timer.start();
        emit changed();
    });
    emit changed();
}

void EndpointSettingsModel::close()
{
    if (m_closed) return;
    m_closed = true; m_timer.stop(); m_pool.waitForDone();
    if (m_revision > m_persistedRevision) {
        try { m_store.saveEndpoints(m_values, m_keys); }
        catch (...) { /* Invalid drafts leave the last valid settings intact. */ }
    }
}

WebhookSettingsModel::WebhookSettingsModel(SettingsStore store, QObject *parent)
    : QObject(parent), m_store(std::move(store)), m_values(m_store.loadWebhook()),
      m_hasKey(m_store.hasCredential("webhook:key"))
{
    m_pool.setMaxThreadCount(1);
    m_timer.setSingleShot(true); m_timer.setInterval(400);
    connect(&m_timer, &QTimer::timeout, this, &WebhookSettingsModel::save);
}

WebhookSettingsModel::~WebhookSettingsModel() { close(); }
bool WebhookSettingsModel::canTest() const
{
    return !m_busy && SettingsStore::validWebhookUrl(m_values["url"].toString().trimmed());
}
bool WebhookSettingsModel::canSave() const
{
    return !m_busy && (!m_values["enabled"].toBool() || SettingsStore::validWebhookUrl(m_values["url"].toString().trimmed()));
}
QString WebhookSettingsModel::message() const
{
    return !m_message.isEmpty() ? m_message : (!canSave() && !m_busy ? QStringLiteral("Enter a valid HTTP or HTTPS webhook URL.") : QString());
}
void WebhookSettingsModel::edited() { ++m_revision; m_message.clear(); m_timer.start(); emit changed(); }
void WebhookSettingsModel::setValue(const QString &name, const QVariant &value)
{
    if (m_closed || !m_values.contains(name)) return;
    m_values[name] = value; edited();
}
void WebhookSettingsModel::editKey(const QString &value) { if (!m_closed) { m_key = value; edited(); } }

void WebhookSettingsModel::activate()
{
    if (m_closed || m_busy) return;
    m_busy = true;
    startOperation(m_pool, this, [store = m_store] {
        return OperationResult{{}, {{"key", store.hasCredential("webhook:key")}}, {}};
    }, [this](const OperationResult &result) {
        if (m_closed) return;
        m_busy = false;
        if (!result.error.isEmpty()) { m_message = result.error; emit errorOccurred(result.error); }
        if (result.error.isEmpty()) m_hasKey = result.status["key"].toBool();
        if (m_revision > m_persistedRevision && result.error.isEmpty()) m_timer.start();
        emit changed();
    });
    emit changed();
}

void WebhookSettingsModel::save()
{
    if (m_closed) return;
    if (m_busy) { m_timer.start(); return; }
    m_timer.stop();
    if (!canSave()) { emit changed(); return; }
    const auto revision = m_revision;
    m_busy = true;
    startOperation(m_pool, this, [store = m_store, values = m_values, key = m_key] {
        const auto saved = store.saveWebhook(values, key);
        return OperationResult{saved, {{"key", store.hasCredential("webhook:key")}}, {}};
    }, [this, revision](const OperationResult &result) {
        if (m_closed) return;
        m_busy = false;
        if (!result.error.isEmpty()) { m_message = result.error; emit errorOccurred(result.error); }
        if (result.error.isEmpty()) {
            m_hasKey = result.status["key"].toBool(); m_persistedRevision = revision;
            if (m_revision == revision) { m_values = result.values; m_key.reset(); }
            emit saved();
        }
        if (m_revision > m_persistedRevision && result.error.isEmpty()) m_timer.start();
        emit changed();
    });
    emit changed();
}

void WebhookSettingsModel::test()
{
    if (m_closed || !canTest()) return;
    m_busy = true; m_message.clear();
    m_testCancel = std::make_shared<std::atomic_bool>(false);
    startOperation(m_pool, this, [store = m_store, values = m_values, key = m_key, cancel = m_testCancel] {
        try {
            notifications::WebhookService(store).test(values, key, cancel);
            return OperationResult{};
        } catch (const notifications::WebhookError &error) {
            return OperationResult{{}, {}, errorText(error)};
        } catch (...) {
            return OperationResult{{}, {}, "Cannot access webhook settings or key in app.db."};
        }
    }, [this](const OperationResult &result) {
        if (m_closed) return;
        m_busy = false;
        m_message = result.error.isEmpty() ? QStringLiteral("Test webhook notification sent.") : result.error;
        if (!result.error.isEmpty()) emit errorOccurred(result.error);
        if (m_revision > m_persistedRevision && result.error.isEmpty()) m_timer.start();
        emit changed();
    });
    emit changed();
}

void WebhookSettingsModel::close()
{
    if (m_closed) return;
    m_closed = true; m_timer.stop(); m_testCancel->store(true); m_pool.waitForDone();
    if (m_revision > m_persistedRevision) {
        try { m_store.saveWebhook(m_values, m_key); }
        catch (...) { /* Invalid drafts never overwrite the last valid settings. */ }
    }
}

SettingsModel::SettingsModel(SettingsStore store, QObject *parent)
    : QObject(parent), m_store(std::move(store)), m_snapshot(m_store.load()), m_detectionResources(m_store.detectionResources()),
      m_visualRefinementEnabled(m_store.visualRefinementEnabled()),
      m_media(new ProviderSettingsModel(m_store, m_snapshot.mediaProvider, this)),
      m_endpoints(new EndpointSettingsModel(m_store, this)), m_webhooks(new WebhookSettingsModel(m_store, this))
{
}
SettingsModel::~SettingsModel() { close(); }
void SettingsModel::close() { m_media->flush(); m_endpoints->close(); m_webhooks->close(); }
void SettingsModel::update(const QString &field, const QVariant &value)
{
    try {
        const auto result = m_store.update(field, value);
        if (result.timestampFormat == m_snapshot.timestampFormat && result.listingType == m_snapshot.listingType
            && result.mediaProvider == m_snapshot.mediaProvider) return;
        m_snapshot = result; emit changed();
    } catch (const SettingsError &failure) { emit error(errorText(failure)); }
    catch (...) { emit error(QStringLiteral("Cannot save INTROvert preferences in app.db.")); }
}
void SettingsModel::setDetectionResources(const QString &value)
{
    try { m_store.setDetectionResources(value); m_detectionResources=value; emit changed(); }
    catch (...) { emit error(QStringLiteral("Cannot save detection resource preferences in app.db.")); }
}
void SettingsModel::setDetectionEffort(const QString &value)
{
    if(value!="light" && value!="medium" && value!="high") {emit error(QStringLiteral("Invalid detection effort."));return;}
    setDetectionResources(value=="light"?"low":value=="high"?"full":"medium");
}
void SettingsModel::setTimestampFormat(const QString &value) { update("timestamp_format", value); }
void SettingsModel::setVisualRefinementEnabled(bool value)
{
    try { m_store.setVisualRefinementEnabled(value); m_visualRefinementEnabled=value; emit changed(); }
    catch (...) { emit error(QStringLiteral("Cannot save visual refinement preference in app.db.")); }
}
void SettingsModel::setListingType(const QString &value) { update("tv_show_listing_type", value); }
void SettingsModel::setMediaProvider(const QString &value) { update("media_provider", value); m_media->select(m_snapshot.mediaProvider); }

} // namespace introvert::ui
