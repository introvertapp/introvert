#include "metadata_service.h"
#include "segments/identity.h"

#include <QDateTime>
#include <algorithm>
#include <QImageReader>
#include <QBuffer>
#include <QJsonObject>
#include <QRegularExpression>

namespace introvert::providers {
namespace {
QVariant nullable(const std::optional<QString> &value) {return value?QVariant(sql::text(*value)):QVariant(QMetaType::fromType<QString>());}
bool compatible(const ExternalIds &before,const ExternalIds &after)
{
    for(const auto &pair:{qMakePair(before.imdb,after.imdb),qMakePair(before.tmdb,after.tmdb),qMakePair(before.tvdb,after.tvdb)})
        if(pair.first && pair.second && pair.first!=pair.second)return false;
    return true;
}
}
MetadataService::MetadataService(QString databasePath,library::MediaCache &cache,MetadataClient client)
    :m_path(std::move(databasePath)),m_provider(m_path),m_cache(cache),m_client(std::move(client)),m_settings(m_path)
{
    m_provider.initializeCatalog();sql::Connection connection(m_path);
    segments::initializeTables(connection.db);
    sql::execute(connection.db,"CREATE TABLE IF NOT EXISTS local_metadata_jobs(root TEXT PRIMARY KEY,status TEXT NOT NULL,current INTEGER NOT NULL,total INTEGER NOT NULL,title TEXT,message TEXT,error TEXT,updated REAL NOT NULL)");
    sql::execute(connection.db,"CREATE TABLE IF NOT EXISTS local_metadata_results(root TEXT PRIMARY KEY,updated_count INTEGER,unmatched INTEGER,failed INTEGER,artwork_failed INTEGER)");
}
QVariantMap MetadataService::status(const QString &root) const
{
    sql::Connection connection(m_path);
    auto query=sql::execute(connection.db,"SELECT root,status,current,total,title,message,error,updated FROM local_metadata_jobs WHERE root=?",{root});
    if(!query.next())return {};
    const QStringList fields{"root","status","current","total","title","message","error","updated"};QVariantMap result;
    for(int i=0;i<fields.size();++i)result[fields[i]]=query.value(i);query.finish();
    auto counts=sql::execute(connection.db,"SELECT updated_count,unmatched,failed,artwork_failed FROM local_metadata_results WHERE root=?",{root});
    if(counts.next()) {const QStringList names{"updated_count","unmatched","failed","artwork_failed"};for(int i=0;i<names.size();++i)result[names[i]]=counts.value(i);}
    return result;
}
QVariantMap MetadataService::record(const QString &root,const QString &state,qint64 current,qint64 total,
    const QString &title,const QString &message,const QString &error) const
{
    sql::Connection connection(m_path);
    sql::execute(connection.db,"INSERT OR REPLACE INTO local_metadata_jobs VALUES(?,?,?,?,?,?,?,?)",
        {root,state,current,total,sql::text(title),sql::text(message),sql::text(error),QDateTime::currentMSecsSinceEpoch()/1000.});
    return status(root);
}
QVariantMap MetadataService::recover(const QString &root) const
{
    const auto value=status(root);
    if(QStringList{"running","queued","canceling"}.contains(value["status"].toString()))
        return record(root,"interrupted",value["current"].toLongLong(),value["total"].toLongLong(),{},"Previous fetch was interrupted. Fetch again to retry.");
    return value;
}
QVariantMap MetadataService::run(const ProviderConfig &config,const Cancellation &cancel,const Progress &progress,const QString &showId)
{
    const auto root=LocalProvider::rootPath(config);qint64 current=0,total=0,updated=0,unmatched=0,failed=0,artworkFailed=0;
    const auto options=m_settings.loadLocalMetadata(root);const auto source=options["provider"].toString(),order=options["episode_order"].toString();
    const auto artworkSource=options["artwork"]=="same"?source:options["artwork"].toString();
    QVariantMap credentials;
    for(const auto &name:{"tmdb","tvdb","tvdb_pin","omdb","fanart"})credentials[name]=m_settings.credential("metadata:"+QString(name)).value_or(QString());
    auto report=[&](const QString &state,const QString &title=QString(),const QString &message=QString(),const QString &error=QString()) {
        {sql::Connection connection(m_path);sql::execute(connection.db,"INSERT OR REPLACE INTO local_metadata_results VALUES(?,?,?,?,?)",{root,updated,unmatched,failed,artworkFailed});}
        const auto result=record(root,state,current,total,title,message,error);if(progress)progress(result);return result;
    };
    try {
        report("running",{},"Loading saved Local inventory");
        if(!m_provider.hasCatalog(config,cancel))return report("failed",{},"Rescan the Local folder before fetching metadata.");
        auto shows=m_provider.listSeries(config,false,cancel);
        if(!showId.isEmpty())shows.erase(std::remove_if(shows.begin(),shows.end(),[&](const auto &show){return show.identity.itemId!=showId;}),shows.end());
        total=shows.size();
        if(!total)return report("complete",{},"Fetch finished: 0 updated, 0 unmatched, 0 failed, 0 artwork errors");
        if(source!="tvmaze" && credentials.value(source).toString().trimmed().isEmpty())
            return report("failed",{},"Save the selected metadata provider's API credentials in Settings → Media.");
        if(config.fetchImages && artworkSource!="off" && artworkSource!="tvmaze" && credentials.value(artworkSource).toString().trimmed().isEmpty())
            return report("failed",{},"Save the selected artwork provider's API credentials or turn artwork Off.");
        for(const auto &show:shows) {
            checkCancelled(cancel);report("running",show.title,"Fetching "+source+": "+show.title);
            try {
                const auto detail=m_provider.getShow(config,show.identity.itemId,false,cancel);
                if(!detail)throw ProviderError("Show was removed from the saved inventory.");
                QString savedId,poster,logo;
                {sql::Connection connection(m_path);
                    auto match=sql::execute(connection.db,"SELECT provider_id FROM local_metadata_matches WHERE show_id=? AND provider=?",{show.identity.itemId,source});
                    if(match.next())savedId=match.value(0).toString();match.finish();
                    auto images=sql::execute(connection.db,"SELECT poster,logo FROM local_show_metadata WHERE show_id=?",{show.identity.itemId});
                    if(images.next()) {poster=images.value(0).toString();logo=images.value(1).toString();}
                }
                auto result=m_client.identify(source,*detail,savedId,order,credentials,cancel,config.fetchImages && artworkSource==source);
                // Preserve known cross-IDs only while the new match corroborates them.
                if(compatible(detail->show.externalIds,result.ids)) {
                    if(!result.ids.imdb)result.ids.imdb=detail->show.externalIds.imdb;
                    if(!result.ids.tmdb)result.ids.tmdb=detail->show.externalIds.tmdb;
                    if(!result.ids.tvdb)result.ids.tvdb=detail->show.externalIds.tvdb;
                    if(!result.ids.tvmaze)result.ids.tvmaze=detail->show.externalIds.tvmaze;
                }
                QString imageError;bool artworkBlocked=false;
                if(config.fetchImages && artworkSource!="off")try {
                    const auto images=m_client.artwork(artworkSource,result,*detail,order,credentials,cancel);
                    const auto scope=m_cache.scope(config,"images");
                    auto cacheImage=[&](const QString &url,QString &previous) {
                        if(url.isEmpty())return;
                        if(!m_cache.image(scope,url)) {
                            auto bytes=MetadataClient::image(url,cancel);QBuffer buffer(&bytes);buffer.open(QIODevice::ReadOnly);QImageReader reader(&buffer);
                            const auto size=reader.size();
                            if(!size.isValid() || size.width()>16384 || size.height()>16384 || qint64(size.width())*size.height()>40000000 || reader.read().isNull())
                                throw ProviderError("Artwork provider returned an invalid or oversized image.");
                            if(!m_cache.putImage(scope,url,bytes))throw ProviderError("Cannot save downloaded artwork to the local image cache.");
                        }
                        previous=url;
                    };
                    cacheImage(images.first,poster);cacheImage(images.second,logo);
                } catch(const ProviderError &error) {++artworkFailed;imageError=QString::fromUtf8(error.what());artworkBlocked=error.httpStatus()==401 || error.httpStatus()==403 || error.httpStatus()==429;}
                checkCancelled(cancel);
                auto after=detail->show;after.title=result.title;after.externalIds=result.ids;
                sql::Connection connection(m_path);auto &db=connection.db;sql::Transaction transaction(db);
                if(result.year)sql::execute(db,"UPDATE shows SET year=COALESCE(year,?) WHERE id=?",{*result.year,show.identity.itemId});
                sql::execute(db,"INSERT OR REPLACE INTO local_show_metadata VALUES(?,?,?,?,?,?,?,?,?,?)",
                    {show.identity.itemId,result.title,nullable(result.ids.imdb),nullable(result.ids.tmdb),nullable(result.ids.tvdb),nullable(result.ids.tvmaze),sql::text(poster),sql::text(logo),
                     imageError.isEmpty()?"complete":"artwork_failed",sql::text(imageError)});
                sql::execute(db,"INSERT INTO local_metadata_matches(show_id,provider,provider_id) VALUES(?,?,?) ON CONFLICT(show_id,provider) DO UPDATE SET provider_id=excluded.provider_id",
                    {show.identity.itemId,source,result.id});
                sql::execute(db,"DELETE FROM local_episode_addresses WHERE episode_id IN(SELECT id FROM episodes WHERE show_id=?)",{show.identity.itemId});
                for(const auto &season:detail->seasons)for(const auto &episode:season.episodes) {
                    if(!episode.seasonNumber || !episode.episodeNumber)continue;
                    const auto found=result.episodes.constFind({*episode.seasonNumber,*episode.episodeNumber});
                    if(found==result.episodes.cend())continue;
                    if(!found->title.isEmpty())sql::execute(db,"INSERT OR REPLACE INTO local_episode_metadata VALUES(?,?,?,?)",{show.identity.itemId,*episode.seasonNumber,*episode.episodeNumber,found->title});
                    sql::execute(db,"INSERT OR REPLACE INTO local_episode_addresses VALUES(?,?,?,?)",{episode.identity.itemId,source=="tmdb"?"tmdb":source=="tvdb" || (!result.ids.imdb && result.ids.tvdb)?"tvdb":"imdb",order,sql::text(found->tvdbId)});
                }
                if(compatible(detail->show.externalIds,result.ids))segments::copyAliases(db,detail->show,after);
                checkCancelled(cancel);transaction.commit();++updated;
                if(artworkBlocked) {++current;return report("failed",{},imageError);}
            } catch(const ProviderError &error) {
                if(dynamic_cast<const MetadataNotFound *>(&error))++unmatched;else ++failed;
                sql::Connection connection(m_path);
                sql::execute(connection.db,"INSERT INTO local_show_metadata(show_id,status,error) VALUES(?,?,?) ON CONFLICT(show_id) DO UPDATE SET status=excluded.status,error=excluded.error",
                    {show.identity.itemId,dynamic_cast<const MetadataNotFound *>(&error)?"not_found":"failed",QString::fromUtf8(error.what())});
                if(error.httpStatus()==401 || error.httpStatus()==403 || error.httpStatus()==429)return report("failed",{},QString::fromUtf8(error.what()));
            }
            ++current;report("running",{},QString("%1 / %2 shows; %3 updated, %4 unmatched, %5 failed, %6 artwork errors").arg(current).arg(total).arg(updated).arg(unmatched).arg(failed).arg(artworkFailed));
        }
        return report(failed || unmatched || artworkFailed?"completed_with_errors":"complete",{},
            QString("Fetch finished: %1 updated, %2 unmatched, %3 failed, %4 artwork errors").arg(updated).arg(unmatched).arg(failed).arg(artworkFailed));
    } catch(const Cancelled &) {return report("canceled",{},"Metadata fetch stopped");}
    catch(...) {return report("failed",{},"Metadata fetch failed","Cannot update Local metadata. Check app.db access and retry.");}
}
} // namespace introvert::providers
