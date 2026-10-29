#include "library_model.h"
#include <QRunnable>

namespace introvert::ui {
using namespace providers;

LocalMetadataModel::LocalMetadataModel(LibraryModel &library, MetadataClient client)
    : QObject(&library), m_library(library), m_store(library.m_store),
      m_service(m_store.databasePath(), library.m_cache, std::move(client))
{
    m_pool.setMaxThreadCount(1);
    m_timer.setSingleShot(true); m_timer.setInterval(400);
    connect(&m_timer, &QTimer::timeout, this, &LocalMetadataModel::flushKey);
}

LocalMetadataModel::~LocalMetadataModel() { close(); }

void LocalMetadataModel::start(Kind kind, Work work)
{
    if (m_closed || m_busy) return;
    m_busy = true; m_fetching = kind == Kind::Fetch;
    const auto generation = ++m_generation;
    m_pool.start(QRunnable::create([this, kind, generation, work = std::move(work)] {
        std::optional<Result> result;
        QString error;
        try { result = work(generation); }
        catch (...) { error = "Cannot access Local metadata or API keys. Check application data access."; }
        QMetaObject::invokeMethod(this, [this, kind, generation, result = std::move(result), error] {
            if (m_closed || generation != m_generation) return;
            m_busy = false; m_message = error;
            if (result) {
                if (kind == Kind::Key) { m_hasKey = result->hasKey; emit keySaved(); }
                else {
                    m_status = result->status;
                    m_message = m_status.value("error").toString();
                    if (m_message.isEmpty()) m_message = m_status.value("message").toString();
                    if (kind == Kind::Status) m_hasKey = result->hasKey;
                    else if (m_library.selection() == m_fetchKey) m_library.refresh();
                }
            }
            emit changed();
        }, Qt::QueuedConnection);
    }));
    emit changed();
}

void LocalMetadataModel::activate()
{
    if (m_closed || m_library.m_settings.mediaProvider() != "local") return;
    try {
        const auto root = m_store.loadProvider("local").localRoot;
        start(Kind::Status, [this, root](quint64) {
            return Result{m_store.hasCredential("metadata:tmdb"), m_service.recover(root)};
        });
    } catch (...) {
        m_message = "Cannot access Local metadata or API keys. Check application data access."; emit changed();
    }
}

void LocalMetadataModel::queueKey(const QString &key)
{
    if (m_closed) return;
    m_pendingKey = key; m_timer.start();
}

void LocalMetadataModel::flushKey()
{
    if (m_closed) return;
    if (m_busy) { m_timer.start(); return; }
    if (m_pendingKey) {
        const auto key = *m_pendingKey; m_pendingKey.reset(); saveKey(key);
    }
}

void LocalMetadataModel::storeKey(const QString &key)
{
    if (key.contains('\r') || key.contains('\n')) throw SettingsError("Invalid API key");
    m_store.setCredential("metadata:tmdb", key.isEmpty() ? std::optional<QString>() : std::optional<QString>(key));
}

void LocalMetadataModel::saveKey(const QString &key)
{
    start(Kind::Key, [this, key](quint64) {
        storeKey(key); return Result{!key.isEmpty(), {}};
    });
}

void LocalMetadataModel::fetch()
{
    if (m_closed || m_busy || m_library.busy() || !m_library.selection()) return;
    const auto key = *m_library.selection();
    if (key.saved.provider != "local") return;
    m_cancel = std::make_shared<std::atomic_bool>(false);
    const auto cancel = m_cancel;
    m_fetchKey = key;
    ProviderConfig config(ProviderId::Local);
    config.localRoot = key.saved.localRoot; config.fetchImages = key.saved.fetchImages;
    const auto pending = m_pendingKey; m_pendingKey.reset(); m_timer.stop();
    start(Kind::Fetch, [this, pending, config, cancel](quint64 generation) {
        if (pending) storeKey(*pending);
        Result result;
        result.status = m_service.run(config, cancel, [this, generation](const QVariantMap &value) {
            QMetaObject::invokeMethod(this, [this, generation, value] {
                if (m_closed || generation != m_generation) return;
                m_status = value; m_message = value.value("message").toString(); emit changed();
            }, Qt::QueuedConnection);
        });
        return result;
    });
}

void LocalMetadataModel::stop()
{
    if (m_closed || !fetching()) return;
    m_cancel->store(true); m_message = "Stopping metadata fetch"; emit changed();
}

void LocalMetadataModel::close()
{
    if (m_closed) return;
    m_closed = true; m_timer.stop(); m_cancel->store(true); m_pool.waitForDone();
    // Flush a last draft on the owning worker without exposing saved keys or
    // allowing a queued completion to restart library work during shutdown.
    if (m_pendingKey) {
        const auto key = *m_pendingKey; m_pendingKey.reset();
        m_pool.start(QRunnable::create([this, key] { try { storeKey(key); } catch (...) {} }));
        m_pool.waitForDone();
    }
}

} // namespace introvert::ui
