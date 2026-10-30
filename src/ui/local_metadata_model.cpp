#include "library_model.h"
#include <QRunnable>
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
                    if(keyRevision==m_keyRevision && !m_pendingCredentials.contains("tmdb"))m_hasKey=result->hasKey;
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
        ProviderConfig config(ProviderId::Local);config.localRoot=m_store.loadProvider("local").localRoot;
        m_root=LocalProvider::rootPath(config);
        const auto options=m_store.loadLocalMetadata(m_root);
        if(options!=m_configuration) {m_configuration=options;emit configurationChanged();}
        for(const auto &name:{"tmdb","tvdb","tvdb_pin","omdb","fanart"}) {
            const QString provider=name;
            m_credentials[provider]=m_pendingCredentials.contains(provider)?m_pendingCredentials[provider]:m_store.credential("metadata:"+provider).value_or(QString());
        }
        m_credential=m_credentials.value("tmdb").toString();m_hasKey=!m_credential.isEmpty();
        if(!m_busy)start(Kind::Status,[this,root=m_root](quint64){return Result{m_store.hasCredential("metadata:tmdb"),m_service.recover(root)};});
        emit changed();
    } catch(...) {m_message="Cannot load Local metadata settings. Check the media folder configuration and app.db access.";emit errorOccurred(m_message);emit changed();}
}

void LocalMetadataModel::setOption(const QString &name,const QString &value)
{
    if(m_closed || fetching() || !m_configuration.contains(name))return;
    try {
        auto values=m_configuration;values[name]=value;
        if(name=="provider" && value!="tvdb")values["episode_order"]="official";
        m_store.saveLocalMetadata(m_root,values);m_configuration=values;m_message.clear();emit configurationChanged();emit changed();
        if(name=="artwork") {
            auto *media=qobject_cast<ProviderSettingsModel *>(m_library.m_settings.mediaSettings());
            media->setFetchImages(value!="off");media->flush();
        }
    } catch(const SettingsError &error) {m_message=QString::fromUtf8(error.what());emit errorOccurred(m_message);emit changed();}
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

void LocalMetadataModel::queueCredential(const QString &provider,const QString &key)
{
    if(m_closed || !QStringList{"tmdb","tvdb","tvdb_pin","omdb","fanart"}.contains(provider))return;
    m_credentials[provider]=key;m_pendingCredentials[provider]=key;
    if(provider=="tmdb") {m_credential=key;++m_keyRevision;}
    m_timer.start();emit changed();
}
void LocalMetadataModel::queueKey(const QString &key) {queueCredential("tmdb",key);}
void LocalMetadataModel::flushKey()
{
    if(m_closed || m_pendingCredentials.isEmpty())return;
    try {
        for(auto i=m_pendingCredentials.begin();i!=m_pendingCredentials.end();++i) {
            if(i.value().contains('\r') || i.value().contains('\n'))throw SettingsError("Invalid API key");
            m_store.setCredential("metadata:"+i.key(),i.value().isEmpty()?std::nullopt:std::optional<QString>(i.value()));
        }
        m_pendingCredentials.clear();m_hasKey=m_store.hasCredential("metadata:tmdb");m_message.clear();emit keySaved();emit changed();
    } catch(...) {m_message="Cannot save metadata credentials. Check API key format and app.db access.";emit errorOccurred(m_message);emit changed();}
}
void LocalMetadataModel::saveKey(const QString &key) {queueKey(key);m_timer.stop();flushKey();}

void LocalMetadataModel::fetch()
{
    if (m_closed || m_busy || m_library.busy() || !m_library.selection()) return;
    const auto key = *m_library.selection();
    if (key.saved.provider != "local") return;
    m_cancel = std::make_shared<std::atomic_bool>(false);
    const auto cancel = m_cancel;
    m_fetchKey = key;
    ProviderConfig config(ProviderId::Local);
    config.localRoot = key.saved.localRoot;
    try {config.fetchImages = m_store.loadLocalMetadata(LocalProvider::rootPath(config)).value("artwork").toString()!="off";}
    catch(...) {m_message="Cannot load Local artwork settings. Check app.db access and retry.";emit errorOccurred(m_message);emit changed();return;}
    m_timer.stop();flushKey();
    if(!m_pendingCredentials.isEmpty())return;
    const auto target=m_targetShow;m_targetShow.clear();
    start(Kind::Fetch, [this, config, cancel, target](quint64 generation) {
        Result result;
        result.status = m_service.run(config, cancel, [this, generation](const QVariantMap &value) {
            QMetaObject::invokeMethod(this, [this, generation, value] {
                if (m_closed || generation != m_generation) return;
                m_status = value; m_message = value.value("message").toString(); emit changed();
            }, Qt::QueuedConnection);
        },target);
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
                    m_store.setCredential("metadata:"+i.key(),i.value().isEmpty()?std::nullopt:std::optional<QString>(i.value()));
            } catch(...) {}
        }));m_pool.waitForDone();
    }
}

} // namespace introvert::ui
