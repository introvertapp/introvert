#include "library_model.h"
#include <QRunnable>
#include <QElapsedTimer>
#include <QRegularExpression>

namespace introvert::ui {
using namespace providers;

LocalMetadataModel::LocalMetadataModel(LibraryModel &library, MetadataClient client)
    : QObject(&library), m_library(library), m_store(library.m_store),
      m_service(m_store.databasePath(), library.m_cache, std::move(client))
{
    m_pool.setMaxThreadCount(1);
    m_timer.setSingleShot(true); m_timer.setInterval(400);
    connect(&m_timer, &QTimer::timeout, this, &LocalMetadataModel::flushKey);
    connect(&library.m_settings,&SettingsModel::changed,this,[this]{if(!fetching())activate();});
    connect(qobject_cast<ProviderSettingsModel *>(library.m_settings.mediaSettings()),&ProviderSettingsModel::saved,this,[this]{if(!fetching())activate();});
}

LocalMetadataModel::~LocalMetadataModel() { close(); }

void LocalMetadataModel::start(Kind kind, Work work)
{
    if (m_closed || m_busy) return;
    m_busy = true; m_fetching = kind == Kind::Fetch;
    const auto generation = ++m_generation;
    const auto keyRevision = m_keyRevision;
    m_pool.start(QRunnable::create([this, kind, generation, keyRevision, work = std::move(work)] {
        std::optional<Result> result;
        QString error;
        try { result = work(generation); }
        catch (...) { error = "Cannot access Local metadata or API keys. Check application data access."; }
        QMetaObject::invokeMethod(this, [this, kind, generation, keyRevision, result = std::move(result), error] {
            if (m_closed || generation != m_generation) return;
            m_busy = false; m_message = error;
            if (!error.isEmpty()) emit errorOccurred(error);
            if (result) {
                m_status=result->status;m_message=m_status.value("error").toString();
                if (!m_message.isEmpty()) emit errorOccurred(m_message);
                if(m_message.isEmpty())m_message=m_status.value("message").toString();
                if(kind==Kind::Status) {
                    if(keyRevision==m_keyRevision && !m_pendingCredentials.contains("metadata:tmdb"))m_hasKey=result->hasKey;
                } else if(m_library.selection()==m_fetchKey)m_library.reloadLocalMetadata();
                else if(m_library.m_settings.mediaProvider()=="local")activate();
            }
            emit changed();
        }, Qt::QueuedConnection);
    }));
    emit changed();
}

void LocalMetadataModel::activate()
{
    if(m_closed || m_library.m_settings.mediaProvider()!="local")return;
    try {
        const auto saved = m_store.loadProvider("local");
        const auto primary = saved.localRoot.isEmpty() ? saved.localPaths("movies").value(0) : saved.localRoot;
        ProviderConfig config(ProviderId::Local);
        const auto *detail = m_library.activeDetail();
        config.localRoot = detail && !detail->show.isMovie() ? detail->show.localRoot.value_or(primary) : primary;
        m_root = primary.isEmpty() ? QString() : LocalProvider::rootPath(config);
        const auto options = m_store.loadLocalLibraryMetadata("tv");
        const auto movies = m_store.loadLocalLibraryMetadata("movies");
        if (options != m_configuration || movies != m_movieConfiguration) {
            m_configuration = options;
            m_movieConfiguration = movies;
            emit configurationChanged();
        }
        for (const auto &name : {"tmdb","tvdb","omdb","fanart"}) {
            const QString provider = name;
            const auto tvAccount = "metadata:" + provider, movieAccount = "metadata:movies:" + provider;
            m_credentials[provider] = m_pendingCredentials.contains(tvAccount) ? m_pendingCredentials[tvAccount]
                : m_store.credential(tvAccount).value_or(QString());
            m_movieCredentials[provider] = m_pendingCredentials.contains(movieAccount) ? m_pendingCredentials[movieAccount]
                : m_store.credential(movieAccount).value_or(QString());
        }
        m_credential=m_credentials.value("tmdb").toString();m_hasKey=!m_credential.isEmpty();
        if(!m_busy && !m_root.isEmpty())start(Kind::Status,[this,root=m_root](quint64){return Result{m_store.hasCredential("metadata:tmdb"),m_service.recover(root)};});
        emit changed();
    } catch(...) {m_message="Cannot load Local metadata settings. Check the media folder configuration and app.db access.";emit errorOccurred(m_message);emit changed();}
}

void LocalMetadataModel::setOption(const QString &name, const QString &value)
{
    setMediaOption("tv", name, value);
}

void LocalMetadataModel::setMovieOption(const QString &name, const QString &value)
{
    setMediaOption("movies", name, value);
}

void LocalMetadataModel::setMediaOption(const QString &type, const QString &name, const QString &value)
{
    if (m_closed || fetching() || !QStringList{"provider","artwork","episode_order"}.contains(name)) return;
    try {
        auto options = m_store.loadLocalLibraryMetadata(type);
        options[name] = value;
        if (type == "movies" || (name == "provider" && value != "tvdb")) options["episode_order"] = "official";
        m_store.saveLocalLibraryMetadata(type, options);
        if (type == "movies") m_movieConfiguration = options;
        else m_configuration = options;
        m_message.clear();
        emit configurationChanged(); emit changed();
        if (name == "artwork") m_library.reloadLocalMetadata();
    } catch (const SettingsError &error) {
        m_message = QString::fromUtf8(error.what()); emit errorOccurred(m_message); emit changed();
    }
}

void LocalMetadataModel::identify(const QString &showId,const QString &providerId)
{
    if(m_closed || m_busy || m_library.busy() || providerId.trimmed().isEmpty())return;
    const auto provider=m_configuration.value("provider").toString();
    static const QRegularExpression numericId("^[0-9]{1,10}$"),imdbId("^tt[0-9]{7,8}$");
    if(!(provider=="omdb"?imdbId:numericId).match(providerId.trimmed()).hasMatch()) {
        m_message="Enter the selected provider's show ID.";emit changed();return;
    }
    try {
        sql::Connection connection(m_store.databasePath());
        auto show=sql::execute(connection.db,"SELECT 1 FROM shows WHERE id=? AND root=?",{showId,m_root});
        if(!show.next())return;show.finish();
        sql::execute(connection.db,"INSERT INTO local_metadata_matches VALUES(?,?,?) ON CONFLICT(show_id,provider) DO UPDATE SET provider_id=excluded.provider_id",
            {showId,provider,providerId.trimmed()});
        m_targetShow=showId;fetch();
    } catch(...) {m_message="Cannot save the show identification. Check app.db access.";emit errorOccurred(m_message);emit changed();}
}

void LocalMetadataModel::queueCredential(const QString &provider, const QString &key)
{
    editCredential(false, provider, key);
}

void LocalMetadataModel::queueMovieCredential(const QString &provider, const QString &key)
{
    editCredential(true, provider, key);
}

void LocalMetadataModel::editCredential(bool movies, const QString &provider, const QString &key)
{
    if (m_closed || !QStringList{"tmdb","tvdb","omdb","fanart"}.contains(provider)) return;
    auto &inputs = movies ? m_movieCredentials : m_credentials;
    inputs[provider] = key;
    m_pendingCredentials[(movies ? "metadata:movies:" : "metadata:") + provider] = key;
    if (!movies && provider == "tmdb") { m_credential = key; ++m_keyRevision; }
    m_timer.start(); emit changed();
}
void LocalMetadataModel::queueKey(const QString &key) {queueCredential("tmdb",key);}
void LocalMetadataModel::flushKey()
{
    if(m_closed || m_pendingCredentials.isEmpty())return;
    try {
        for(auto i=m_pendingCredentials.begin();i!=m_pendingCredentials.end();++i) {
            if(i.value().contains('\r') || i.value().contains('\n'))throw SettingsError("Invalid API key");
            m_store.setCredential(i.key(),i.value().isEmpty()?std::nullopt:std::optional<QString>(i.value()));
        }
        m_pendingCredentials.clear();m_hasKey=m_store.hasCredential("metadata:tmdb");m_message.clear();emit keySaved();emit changed();
    } catch(...) {m_message="Cannot save metadata credentials. Check API key format and app.db access.";emit errorOccurred(m_message);emit changed();}
}
void LocalMetadataModel::saveKey(const QString &key) {queueKey(key);m_timer.stop();flushKey();}

void LocalMetadataModel::fetch() { fetchMedia({}); }

void LocalMetadataModel::fetchMedia(const QString &type)
{
    if (!type.isEmpty() && type != "tv" && type != "movies") return;
    if (m_closed || m_busy || m_library.busy()) return;
    auto *media = qobject_cast<ProviderSettingsModel *>(m_library.m_settings.mediaSettings());
    media->flush();
    if (media->dirty() || m_busy || m_library.busy() || !m_library.selection()) return;
    const auto key = *m_library.selection();
    if (key.saved.provider != "local") return;
    m_cancel = std::make_shared<std::atomic_bool>(false);
    const auto cancel = m_cancel;
    m_fetchKey = key;
    m_timer.stop();flushKey();
    if(!m_pendingCredentials.isEmpty())return;
    const auto target = type == "movies" ? QString() : m_targetShow;
    m_targetShow.clear();
    auto folders = key.saved.localFolders;
    if (!type.isEmpty()) folders.removeIf([&](const LocalFolder &folder) { return folder.type != type; });
    if (!target.isEmpty()) {
        const auto *detail = m_library.activeDetail();
        folders = {{detail && detail->show.identity.itemId == target
            ? detail->show.localRoot.value_or(key.saved.localRoot) : key.saved.localRoot, "tv"}};
    }
    if (folders.isEmpty()) return;
    start(Kind::Fetch, [this, folders, cancel, target, type](quint64 generation) {
        Result result;
        QStringList summaries, errors;
        bool hadSuccess = false, hadIssues = false;
        for (qsizetype index = 0; index < folders.size(); ++index) {
            if (cancel->load()) break;
            const auto &row = folders.at(index);
            const bool movies = row.type == "movies";
            ProviderConfig folder(ProviderId::Local);
            folder.localRoot = row.path;
            folder.fetchImages = m_store.loadLocalMetadata(LocalProvider::rootPath(folder), row.type).value("artwork").toString() != "off";
            const auto label = folders.size() > 1 ? row.path + ": " : QString();
            QElapsedTimer publication;
            const auto publish = [this, generation, label, index, count = folders.size(), mediaType = row.type, &publication](QVariantMap value) {
                // Committed items must reach the library even when progress text is throttled.
                if (value.value("item_id").toString().isEmpty() && value.value("status") == "running"
                    && publication.isValid() && publication.elapsed() < 75) return;
                publication.start();
                value["type"] = mediaType;
                value["message"] = label + value.value("message").toString();
                const auto fraction = value.value("current").toDouble() / std::max(1., value.value("total").toDouble());
                value["progress"] = (index + fraction) / count;
                QMetaObject::invokeMethod(this, [this, generation, value] {
                    if (m_closed || generation != m_generation) return;
                    if (m_library.selection() == m_fetchKey && !value.value("item_id").toString().isEmpty())
                        m_library.reloadLocalMetadataItem(value.value("type").toString(), value.value("root").toString(), value.value("item_id").toString());
                    m_status = value; m_message = value.value("message").toString(); emit changed();
                }, Qt::QueuedConnection);
            };
            result.status = movies ? m_service.runMovies(folder, cancel, publish, true)
                                   : m_service.run(folder, cancel, publish, target, target.isEmpty());
            summaries.append(label + result.status.value("message").toString());
            const auto error = result.status.value("error").toString();
            if (!error.isEmpty()) errors.append(label + error);
            const auto state = result.status.value("status").toString();
            hadIssues |= state != "complete";
            hadSuccess |= state == "complete" || state == "completed_with_errors";
        }
        result.status["type"] = type;
        result.status["error"] = errors.join('\n');
        if (cancel->load()) {
            result.status["status"] = "canceled";
            result.status["message"] = "Metadata fetch stopped";
        } else if (folders.size() > 1) {
            result.status["status"] = hadIssues ? (hadSuccess ? "completed_with_errors" : "failed") : "complete";
            result.status["message"] = summaries.join('\n');
            result.status.remove("total");
        }
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
    if(!m_pendingCredentials.isEmpty()) {
        const auto pending=m_pendingCredentials;m_pendingCredentials.clear();
        m_pool.start(QRunnable::create([this,pending] {
            for(auto i=pending.begin();i!=pending.end();++i)try {
                if(!i.value().contains('\r') && !i.value().contains('\n'))
                    m_store.setCredential(i.key(),i.value().isEmpty()?std::nullopt:std::optional<QString>(i.value()));
            } catch(...) {}
        }));m_pool.waitForDone();
    }
}

} // namespace introvert::ui
