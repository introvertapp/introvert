#include "metadata_client.h"
#include "http_transport.h"

#include <QCryptographicHash>
#include <QJsonParseError>
#include <algorithm>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QThread>
#include <QUrlQuery>
#include <cmath>

namespace introvert::providers {
namespace {
QString id(const QJsonValue &value)
{
    if(value.isString())return value.toString();
    const auto n=MetadataClient::positiveId(value);return n?QString::number(*n):QString();
}
QString normalized(QString value)
{
    value=value.toCaseFolded();value.replace(QRegularExpression("[^\\p{L}\\p{N}]+")," ");return value.trimmed();
}
QString imageUrl(const QString &value)
{
    const QUrl url(value,QUrl::StrictMode);
    if(!url.isValid() || !QStringList{"https","http"}.contains(url.scheme()) || url.host().isEmpty()
        || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment())return {};
    return value;
}
QJsonObject choose(const QJsonArray &values,const ShowSummary &show,const QString &titleField,const QString &yearField)
{
    QList<QJsonObject> matches;
    for(const auto &entry:values) {
        auto row=entry.toObject();
        if(row.contains("show"))row=row["show"].toObject();
        if(normalized(row[titleField].toString())!=normalized(show.title))continue;
        const auto year=row[yearField].toString().left(4).toLongLong();
        if(show.year && year && year!=*show.year)continue;
        matches.append(row);
    }
    if(matches.isEmpty())throw MetadataNotFound();
    if(matches.size()!=1)throw ProviderError("Multiple shows match. Set the provider show ID using Identify on the show page.");
    return matches.first();
}
void wait(double seconds,const Cancellation &cancel)
{
    QElapsedTimer timer;timer.start();
    while(timer.elapsed()<seconds*1000) {checkCancelled(cancel);QThread::msleep(10);}
}
}

MetadataClient::Record MetadataClient::identify(const QString &source,const ShowDetail &show,const QString &savedId,
    const QString &order,const QVariantMap &credentials,const Cancellation &cancel,bool includeArtwork) const
{
    if(!QStringList{"tmdb","tvdb","omdb","tvmaze"}.contains(source))throw ProviderError("Unsupported metadata provider.");
    if(source!="tvdb" && order!="official")throw ProviderError("This provider supports aired episode order. Select TheTVDB for alternate orders.");
    const auto key=credentials.value(source).toString().trimmed();
    if(source!="tvmaze" && key.isEmpty())throw ProviderError("Save the selected provider's API credentials in Settings → Media.");
    const auto root=m_bases.value(source).toString();
    QMap<QByteArray,QByteArray> headers{{"Accept","application/json"},{"User-Agent","INTROvert"}};
    auto request=[&](const QString &path,QVariantMap query=QVariantMap(),std::optional<QJsonObject> body=std::nullopt) {
        QUrl url(root+path);QUrlQuery parameters;
        for(auto i=query.begin();i!=query.end();++i)parameters.addQueryItem(i.key(),i.value().toString());
        url.setQuery(parameters);
        for(int attempt=0;;++attempt) {
            wait(m_delay,cancel);
            detail::HttpExchange response;
            try {response=detail::exchange(url,headers,10,20*1024*1024,cancel,
                body?std::optional<QByteArray>(QJsonValue(*body).toJson(QJsonValue::JsonFormat::Compact)):std::nullopt,true);}
            catch(const detail::HttpFailure &) {throw ProviderError("Metadata provider could not be reached. Check network access and retry.");}
            if((response.status==429 || response.status>=500) && attempt<2) {
                bool valid=false;const auto retry=response.headers.value("retry-after").toDouble(&valid);
                if(valid && retry>30)throw ProviderError("Metadata provider is rate limited. Retry later.",429);
                wait(valid?std::max(1.,retry):double(1<<attempt),cancel);continue;
            }
            if(response.status==404)throw MetadataNotFound();
            if(response.status==401 && source=="tvdb" && headers.contains("Authorization") && attempt<2) {
                QJsonObject login{{"apikey",key}};const auto pin=credentials.value("tvdb_pin").toString();if(!pin.isEmpty())login["pin"]=pin;
                detail::HttpExchange refreshed;
                try {refreshed=detail::exchange(QUrl(root+"/login"),{{"Content-Type","application/json"},{"Accept","application/json"}},10,1024*1024,cancel,QJsonValue(login).toJson(QJsonValue::JsonFormat::Compact),true);}
                catch(const detail::HttpFailure &) {throw ProviderError("TheTVDB authentication could not be renewed. Retry later.");}
                m_tvdbToken=QJsonValue::fromJson(refreshed.data).toObject()["data"].toObject()["token"].toString();
                if(refreshed.status==200 && !m_tvdbToken.isEmpty()) {headers["Authorization"]="Bearer "+m_tvdbToken.toUtf8();continue;}
                m_tvdbAuth.clear();
            }
            if(response.status==401 || response.status==403)throw ProviderError("Metadata provider rejected the API key or subscriber PIN.",response.status);
            if(response.status!=200)throw ProviderError("Metadata provider request failed. Check network access or quota and retry.",response.status);
            QJsonParseError error;const auto result=QJsonValue::fromJson(response.data,&error);
            if(error.error!=QJsonParseError::NoError)throw ProviderError("Metadata provider returned invalid JSON.");
            return result;
        }
    };
    Record result;result.source=source;
    if(source=="tmdb") {
        auto tmdbRead=[&](const QString &path,QVariantMap query=QVariantMap()) {query["api_key"]=key;return request(path,query);};
        auto matchId=savedId;
        if(matchId.isEmpty())matchId=show.show.externalIds.tmdb.value_or(QString());
        if(matchId.isEmpty())matchId=id(choose(tmdbRead("/search/tv",{{"query",show.show.title}}).toObject()["results"].toArray(),show.show,"name","first_air_date")["id"]);
        const auto body=tmdbRead("/tv/"+QUrl::toPercentEncoding(matchId),{{"append_to_response",includeArtwork?"external_ids,images":"external_ids"}}).toObject();
        result.id=id(body["id"]);result.title=body["name"].toString();result.ids.tmdb=result.id;
        const auto year=body["first_air_date"].toString().left(4).toLongLong();if(year>0)result.year=year;
        const auto ids=body["external_ids"].toObject();
        if(!id(ids["imdb_id"]).isEmpty())result.ids.imdb=id(ids["imdb_id"]);
        if(!id(ids["tvdb_id"]).isEmpty())result.ids.tvdb=id(ids["tvdb_id"]);
        const auto images=body["images"].toObject();
        if(!body["poster_path"].toString().isEmpty())result.poster="https://image.tmdb.org/t/p/w500"+body["poster_path"].toString();
        for(const auto &value:images["logos"].toArray()) {
            const auto row=value.toObject();if(row["iso_639_1"].toString()=="en" || row["iso_639_1"].isNull()) {
                result.logo="https://image.tmdb.org/t/p/w500"+row["file_path"].toString();break;
            }
        }
        for(const auto &season:show.seasons) {
            if(!season.number)continue;
            const auto body=tmdbRead("/tv/"+matchId+"/season/"+QString::number(*season.number)).toObject();
            for(const auto &value:body["episodes"].toArray()) {
                const auto row=value.toObject();
                result.episodes[{row["season_number"].toInteger(),row["episode_number"].toInteger()}]={row["name"].toString(),{}};
            }
        }
    } else if(source=="tvmaze") {
        auto matchId=savedId;if(matchId.isEmpty())matchId=show.show.externalIds.tvmaze.value_or(QString());
        if(matchId.isEmpty())matchId=id(choose(request("/search/shows",{{"q",show.show.title}}).toArray(),show.show,"name","premiered")["id"]);
        const auto body=request("/shows/"+matchId,show.seasons.isEmpty()?QVariantMap():QVariantMap{{"embed","episodes"}}).toObject();
        result.id=id(body["id"]);result.title=body["name"].toString();result.ids.tvmaze=result.id;
        const auto year=body["premiered"].toString().left(4).toLongLong();if(year>0)result.year=year;
        const auto ids=body["externals"].toObject();
        if(!id(ids["imdb"]).isEmpty())result.ids.imdb=id(ids["imdb"]);
        if(!id(ids["thetvdb"]).isEmpty())result.ids.tvdb=id(ids["thetvdb"]);
        const auto image=body["image"].toObject();result.poster=image["medium"].toString();
        if(result.poster.isEmpty())result.poster=image["original"].toString();
        if(!show.seasons.isEmpty() && (!body["_embedded"].isObject() || !body["_embedded"].toObject()["episodes"].isArray()))throw ProviderError("TVmaze returned an invalid episode list.");
        for(const auto &value:body["_embedded"].toObject()["episodes"].toArray()) {
            if(!value.isObject())throw ProviderError("TVmaze returned an invalid episode record.");
            const auto row=value.toObject();if(row["season"].isDouble() && row["number"].isDouble())
                result.episodes[{row["season"].toInteger(),row["number"].toInteger()}]={row["name"].toString(),{}};
        }
    } else if(source=="omdb") {
        auto omdbRead=[&](QVariantMap query) {query["apikey"]=key;const auto body=request("/",query).toObject();
            if(body["Response"]=="False")throw ProviderError("OMDb could not identify the show or its API quota was exhausted.");return body;};
        auto matchId=savedId;if(matchId.isEmpty())matchId=show.show.externalIds.imdb.value_or(QString());
        if(matchId.isEmpty())matchId=choose(omdbRead({{"s",show.show.title},{"type","series"}})["Search"].toArray(),show.show,"Title","Year")["imdbID"].toString();
        const auto body=omdbRead({{"i",matchId},{"type","series"}});
        result.id=body["imdbID"].toString();result.ids.imdb=result.id;result.title=body["Title"].toString();result.poster=body["Poster"].toString();
        const auto year=body["Year"].toString().left(4).toLongLong();if(year>0)result.year=year;
        for(const auto &season:show.seasons) {
            if(!season.number)continue;const auto body=omdbRead({{"i",result.id},{"Season",*season.number}});
            for(const auto &value:body["Episodes"].toArray()) {
                const auto row=value.toObject();result.episodes[{*season.number,id(row["Episode"]).toLongLong()}]={row["Title"].toString(),{}};
            }
        }
    } else {
        const auto auth=QString::fromLatin1(QCryptographicHash::hash((key+":"+credentials.value("tvdb_pin").toString()).toUtf8(),QCryptographicHash::Sha256).toHex());
        if(m_tvdbToken.isEmpty() || m_tvdbAuth!=auth) {
            QJsonObject login{{"apikey",key}};const auto pin=credentials.value("tvdb_pin").toString();if(!pin.isEmpty())login["pin"]=pin;
            headers["Content-Type"]="application/json";
            m_tvdbToken=request("/login",{},login).toObject()["data"].toObject()["token"].toString();m_tvdbAuth=auth;
            if(m_tvdbToken.isEmpty())throw ProviderError("TheTVDB login returned no token.");
        }
        headers["Authorization"]="Bearer "+m_tvdbToken.toUtf8();
        auto matchId=savedId;if(matchId.isEmpty())matchId=show.show.externalIds.tvdb.value_or(QString());
        if(matchId.isEmpty())matchId=id(choose(request("/search",{{"query",show.show.title},{"type","series"}}).toObject()["data"].toArray(),show.show,"name","year")["tvdb_id"]);
        const auto body=request("/series/"+matchId+"/extended").toObject()["data"].toObject();
        result.id=id(body["id"]);result.title=body["name"].toString();result.ids.tvdb=result.id;
        const auto year=body["firstAired"].toString().left(4).toLongLong();if(year>0)result.year=year;
        for(const auto &value:body["remoteIds"].toArray()) {
            const auto row=value.toObject();const auto name=row["sourceName"].toString().toLower();
            if(name.contains("imdb"))result.ids.imdb=id(row["id"]);
            else if(name.contains("moviedb") || name.contains("tmdb"))result.ids.tmdb=id(row["id"]);
        }
        result.poster=body["image"].toString();
        for(const auto &value:body["artworks"].toArray()) {
            const auto row=value.toObject();if(row["type"].toInt()==23) {result.logo=row["image"].toString();break;}
        }
        for(int page=0;!show.seasons.isEmpty() && page<1000;++page) {
            const auto response=request("/series/"+matchId+"/episodes/"+order,{{"page",page}}).toObject();
            for(const auto &value:response["data"].toObject()["episodes"].toArray()) {
                const auto row=value.toObject();result.episodes[{row["seasonNumber"].toInteger(),row["number"].toInteger()}]={row["name"].toString(),id(row["id"])};
            }
            if(response["links"].toObject()["next"].isNull() || response["links"].toObject()["next"].toString().isEmpty())break;
            if(page==999)throw ProviderError("TheTVDB episode list exceeded the supported page limit.");
        }
    }
    auto checkId=[](std::optional<QString> &value,bool imdbId) {
        if(value && value->isEmpty())value.reset();
        if(value && !QRegularExpression(imdbId?"^tt[0-9]{7,8}$":"^[1-9][0-9]{0,9}$").match(*value).hasMatch())
            throw ProviderError("Metadata provider returned an invalid external identifier.");
    };
    checkId(result.ids.imdb,true);checkId(result.ids.tmdb,false);checkId(result.ids.tvdb,false);
    if(result.id.isEmpty() || result.title.isEmpty())throw ProviderError("Metadata provider returned an invalid show record.");
    result.poster=imageUrl(result.poster);result.logo=imageUrl(result.logo);return result;
}

QPair<QString,QString> MetadataClient::artwork(const QString &source,const Record &record,const ShowDetail &show,
    const QString &order,const QVariantMap &credentials,const Cancellation &cancel) const
{
    if(source=="off")return {};
    auto match=show;match.show.externalIds=record.ids;match.seasons.clear();
    if(source!="fanart") {
        const auto art=record.source==source?record:identify(source,match,{},source=="tvdb"?order:QString("official"),credentials,cancel,true);
        if(source=="tvmaze") {
            QString logo;
            const auto images=tv("/shows/"+art.id+"/images",{},cancel);
            if(!images.isArray())throw ProviderError("TVmaze returned an invalid artwork list.");
            for(const auto &value:images.toArray()) {
                const auto row=value.toObject();if(row["type"]=="typography") {
                    logo=imageUrl(row["resolutions"].toObject()["original"].toObject()["url"].toString());break;
                }
            }
            return {art.poster,logo};
        }
        return {art.poster,art.logo};
    }
    const auto key=credentials.value("fanart").toString().trimmed();
    if(key.isEmpty())throw ProviderError("Save a fanart.tv personal API key in Settings → Media.");
    QString target,origin;
    if(record.ids.tvdb) {target=*record.ids.tvdb;origin="tvdb";}
    else if(record.ids.tmdb) {target=*record.ids.tmdb;origin="tmdb";}
    else if(record.ids.imdb) {target=*record.ids.imdb;origin="imdb";}
    else throw ProviderError("The selected show has no identifier supported by fanart.tv.");
    QUrl url(m_bases.value("fanart").toString()+"/tv/"+QUrl::toPercentEncoding(target));
    QUrlQuery query;query.addQueryItem("source",origin);url.setQuery(query);
    wait(m_delay,cancel);
    detail::HttpExchange response;
    try {response=detail::exchange(url,{{"client-key",key.toUtf8()},{"Accept","application/json"},{"User-Agent","INTROvert"}},10,20*1024*1024,cancel,{},true);}
    catch(const detail::HttpFailure &) {throw ProviderError("fanart.tv could not be reached. Check network access and retry.");}
    if(response.status!=200)throw ProviderError("fanart.tv request failed. Check API access or retry later.",response.status);
    const auto value=QJsonValue::fromJson(response.data);
    if(!value.isObject())throw ProviderError("fanart.tv returned an invalid artwork response.");
    const auto body=value.toObject();
    auto pick=[&](const QStringList &kinds) {
        for(const auto &kind:kinds)for(const auto &value:body[kind].toArray()) {
            const auto row=value.toObject();if(QStringList{"en","00",""}.contains(row["lang"].toString())) {
                const auto url=imageUrl(row["url"].toString());if(!url.isEmpty())return url;
            }
        }
        return QString();
    };
    return {pick({"tvposter"}),pick({"hdtvlogo","clearlogo"})};
}
} // namespace introvert::providers
