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
QJsonObject choose(const QJsonArray &values,const QString &title,std::optional<qint64> wantedYear,
                   const QString &titleField,const QString &yearField,bool movie)
{
    QList<QJsonObject> matches;
    const auto wantedTitle = normalized(title);
    for(const auto &entry:values) {
        auto row=entry.toObject();
        if(row.contains("show"))row=row["show"].toObject();
        if(normalized(row[titleField].toString())!=wantedTitle
            && (!movie || normalized(row["original_title"].toString()) != wantedTitle))continue;
        const auto year=id(row[yearField]).left(4).toLongLong();
        if(wantedYear && ((movie && !year) || (year && year!=*wantedYear)))continue;
        matches.append(row);
    }
    if(matches.isEmpty())throw MetadataNotFound(movie);
    if(matches.size()!=1)throw ProviderError(movie ? "Multiple movies match. Include the release year in the filename."
        : "Multiple shows match. Set the provider show ID using Identify on the show page.");
    return matches.first();
}
void wait(double seconds,const Cancellation &cancel)
{
    QElapsedTimer timer;timer.start();
    while(timer.elapsed()<seconds*1000) {checkCancelled(cancel);QThread::msleep(10);}
}
}

QPair<QString,QString> MetadataClient::sources(const QVariantMap &settings, bool movie)
{
    auto metadata = settings.value("provider").toString();
    auto artwork = settings.value("artwork").toString();
    if (movie && metadata == "tvmaze") metadata = "tmdb";
    if (artwork == "same" || (movie && artwork == "tvmaze")) artwork = metadata;
    return {metadata, artwork};
}

MetadataClient::Record MetadataClient::identify(const QString &source,const ShowDetail &show,const QString &savedId,
    const QString &order,const QVariantMap &credentials,const Cancellation &cancel,bool includeArtwork) const
{
    Title title{show.show.title, show.show.year, show.show.externalIds, {}};
    for (const auto &season : show.seasons) if (season.number) title.seasons.append(*season.number);
    return identifyTitle(source, title, savedId, order, credentials, cancel, includeArtwork);
}

MetadataClient::Record MetadataClient::identifyMovie(const QString &source, const MovieSummary &movie,
    const QVariantMap &credentials, const Cancellation &cancel, bool includeArtwork) const
{
    return identifyTitle(source, {movie.title, movie.year, movie.externalIds, {}, true}, {}, "official", credentials, cancel, includeArtwork);
}

MetadataClient::Record MetadataClient::identifyTitle(const QString &source, const Title &title, const QString &savedId,
    const QString &order, const QVariantMap &credentials, const Cancellation &cancel, bool includeArtwork) const
{
    const bool movie = title.movie;
    const auto chooseTitle = [&](const QJsonArray &rows, const QString &name, const QString &year) {
        return choose(rows, title.name, title.year, name, year, movie);
    };
    if(!QStringList{"tmdb","tvdb","omdb","tvmaze"}.contains(source) || (movie && source == "tvmaze"))throw ProviderError("Unsupported metadata provider.");
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
            if(response.status==404)throw MetadataNotFound(movie);
            if(response.status==401 && source=="tvdb" && headers.contains("Authorization") && attempt<2) {
                QJsonObject login{{"apikey",key}};
                detail::HttpExchange refreshed;
                try {refreshed=detail::exchange(QUrl(root+"/login"),{{"Content-Type","application/json"},{"Accept","application/json"}},10,1024*1024,cancel,QJsonValue(login).toJson(QJsonValue::JsonFormat::Compact),true);}
                catch(const detail::HttpFailure &) {throw ProviderError("TheTVDB authentication could not be renewed. Retry later.");}
                m_tvdbToken=QJsonValue::fromJson(refreshed.data).toObject()["data"].toObject()["token"].toString();
                if(refreshed.status==200 && !m_tvdbToken.isEmpty()) {headers["Authorization"]="Bearer "+m_tvdbToken.toUtf8();continue;}
                m_tvdbAuth.clear();
            }
            if(response.status==401 || response.status==403)throw ProviderError("Metadata provider rejected the API key.",response.status);
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
        if(matchId.isEmpty())matchId=title.ids.tmdb.value_or(QString());
        const QString media = movie ? "/movie" : "/tv";
        const QString titleField = movie ? "title" : "name";
        const QString dateField = movie ? "release_date" : "first_air_date";
        if (matchId.isEmpty() && title.ids.imdb) {
            const auto found = tmdbRead("/find/" + *title.ids.imdb, {{"external_source", "imdb_id"}}).toObject();
            const auto rows = found[movie ? "movie_results" : "tv_results"].toArray();
            if (rows.size() == 1) matchId = id(rows.first().toObject()["id"]);
        }
        if (matchId.isEmpty()) {
            QVariantMap search{{"query", title.name}};
            if (movie && title.year) search["year"] = *title.year;
            matchId = id(chooseTitle(tmdbRead("/search" + media, search).toObject()["results"].toArray(), titleField, dateField)["id"]);
        }
        QVariantMap details{{"append_to_response",includeArtwork ? "external_ids,images" : "external_ids"}};
        if (includeArtwork) details["include_image_language"] = "en,null";
        const auto body = tmdbRead(media + "/" + QUrl::toPercentEncoding(matchId), details).toObject();
        result.id = id(body["id"]); result.title = body[titleField].toString(); result.ids.tmdb = result.id;
        const auto year = body[dateField].toString().left(4).toLongLong(); if (year > 0) result.year = year;
        const auto ids = body["external_ids"].toObject();
        const auto imdb = id(ids["imdb_id"]).isEmpty() ? id(body["imdb_id"]) : id(ids["imdb_id"]);
        if (!imdb.isEmpty()) result.ids.imdb = imdb;
        if (!id(ids["tvdb_id"]).isEmpty()) result.ids.tvdb = id(ids["tvdb_id"]);
        const auto imageBase = m_bases.value("tmdb_images").toString();
        const auto poster = body["poster_path"].toString();
        if (poster.startsWith('/')) result.poster = imageBase + poster;
        const auto logos = body["images"].toObject()["logos"].toArray();
        for (const auto &language : {QString("en"), QString()}) {
            for (const auto &value : logos) {
                const auto row = value.toObject();
                const auto path = row["file_path"].toString();
                if (row["iso_639_1"].toString() == language && path.startsWith('/')) {
                    const bool original = path.endsWith(".svg", Qt::CaseInsensitive) && imageBase.endsWith("/w500");
                    result.logo = (original ? imageBase.chopped(4) + "original" : imageBase) + path;
                    break;
                }
            }
            if (!result.logo.isEmpty()) break;
        }
        for(const auto &season:title.seasons) {
            const auto body=tmdbRead("/tv/"+matchId+"/season/"+QString::number(season)).toObject();
            for(const auto &value:body["episodes"].toArray()) {
                const auto row=value.toObject();
                result.episodes[{row["season_number"].toInteger(),row["episode_number"].toInteger()}]={row["name"].toString(),{}};
            }
        }
    } else if(source=="tvmaze") {
        auto matchId=savedId;if(matchId.isEmpty())matchId=title.ids.tvmaze.value_or(QString());
        if(matchId.isEmpty())matchId=id(chooseTitle(request("/search/shows",{{"q",title.name}}).toArray(),"name","premiered")["id"]);
        const auto body=request("/shows/"+matchId,title.seasons.isEmpty()?QVariantMap():QVariantMap{{"embed","episodes"}}).toObject();
        result.id=id(body["id"]);result.title=body["name"].toString();result.ids.tvmaze=result.id;
        const auto year=body["premiered"].toString().left(4).toLongLong();if(year>0)result.year=year;
        const auto ids=body["externals"].toObject();
        if(!id(ids["imdb"]).isEmpty())result.ids.imdb=id(ids["imdb"]);
        if(!id(ids["thetvdb"]).isEmpty())result.ids.tvdb=id(ids["thetvdb"]);
        const auto image=body["image"].toObject();result.poster=image["medium"].toString();
        if(result.poster.isEmpty())result.poster=image["original"].toString();
        if(!title.seasons.isEmpty() && (!body["_embedded"].isObject() || !body["_embedded"].toObject()["episodes"].isArray()))throw ProviderError("TVmaze returned an invalid episode list.");
        for(const auto &value:body["_embedded"].toObject()["episodes"].toArray()) {
            if(!value.isObject())throw ProviderError("TVmaze returned an invalid episode record.");
            const auto row=value.toObject();if(row["season"].isDouble() && row["number"].isDouble())
                result.episodes[{row["season"].toInteger(),row["number"].toInteger()}]={row["name"].toString(),{}};
        }
    } else if(source=="omdb") {
        const QString type = movie ? "movie" : "series";
        auto omdbRead=[&](QVariantMap query) {query["apikey"]=key;const auto body=request("/",query).toObject();
            if(body["Response"]=="False")throw ProviderError("OMDb could not identify the show or its API quota was exhausted.");return body;};
        auto matchId=savedId;if(matchId.isEmpty())matchId=title.ids.imdb.value_or(QString());
        if (matchId.isEmpty()) {
            QVariantMap search{{"s", title.name}, {"type", type}};
            if (movie && title.year) search["y"] = *title.year;
            matchId = chooseTitle(omdbRead(search)["Search"].toArray(), "Title", "Year")["imdbID"].toString();
        }
        const auto body=omdbRead({{"i",matchId},{"type",type}});
        result.id=body["imdbID"].toString();result.ids.imdb=result.id;result.title=body["Title"].toString();result.poster=body["Poster"].toString();
        const auto year=body["Year"].toString().left(4).toLongLong();if(year>0)result.year=year;
        for(const auto &season:title.seasons) {
            const auto body=omdbRead({{"i",result.id},{"Season",season}});
            for(const auto &value:body["Episodes"].toArray()) {
                const auto row=value.toObject();result.episodes[{season,id(row["Episode"]).toLongLong()}]={row["Title"].toString(),{}};
            }
        }
    } else {
        const auto auth=QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(),QCryptographicHash::Sha256).toHex());
        if(m_tvdbToken.isEmpty() || m_tvdbAuth!=auth) {
            QJsonObject login{{"apikey",key}};
            headers["Content-Type"]="application/json";
            m_tvdbToken=request("/login",{},login).toObject()["data"].toObject()["token"].toString();m_tvdbAuth=auth;
            if(m_tvdbToken.isEmpty())throw ProviderError("TheTVDB login returned no token.");
        }
        headers["Authorization"]="Bearer "+m_tvdbToken.toUtf8();
        auto matchId=savedId;if(matchId.isEmpty())matchId=title.ids.tvdb.value_or(QString());
        if (matchId.isEmpty()) {
            QVariantMap search{{"query", title.name}, {"type", movie ? "movie" : "series"}};
            if (movie && title.year) search["year"] = *title.year;
            matchId = id(chooseTitle(request("/search", search).toObject()["data"].toArray(), "name", "year")["tvdb_id"]);
        }
        const QString media = movie ? "/movies/" : "/series/";
        const auto body = request(media + matchId + "/extended").toObject()["data"].toObject();
        result.id=id(body["id"]);result.title=body["name"].toString();result.ids.tvdb=result.id;
        const auto year=id(body[movie ? "year" : "firstAired"]).left(4).toLongLong();if(year>0)result.year=year;
        for(const auto &value:body["remoteIds"].toArray()) {
            const auto row=value.toObject();const auto name=row["sourceName"].toString().toLower();
            if(name.contains("imdb"))result.ids.imdb=id(row["id"]);
            else if(name.contains("moviedb") || name.contains("tmdb"))result.ids.tmdb=id(row["id"]);
        }
        result.poster=body["image"].toString();
        const auto images = body["artworks"].toArray();
        if (includeArtwork && movie && !images.isEmpty() && !m_tvdbMovieLogoTypes) {
            try {
                QSet<int> types;
                for (const auto &value : request("/artwork/types").toObject()["data"].toArray()) {
                    const auto row = value.toObject();
                    if (row["recordType"].toString().contains("movie", Qt::CaseInsensitive)
                        && row["name"].toString().contains("logo", Qt::CaseInsensitive)) types.insert(row["id"].toInt());
                }
                m_tvdbMovieLogoTypes = types;
            } catch (const ProviderError &error) {
                checkCancelled(cancel);
                result.artworkError = QString::fromUtf8(error.what());
            }
        }
        for (const auto &value : images) {
            const auto row = value.toObject();
            const auto type = row["type"].toInt();
            if (movie ? m_tvdbMovieLogoTypes && m_tvdbMovieLogoTypes->contains(type) : type == 23) {
                result.logo = imageUrl(row["image"].toString());
                if (!result.logo.isEmpty()) break;
            }
        }
        for(int page=0;!title.seasons.isEmpty() && page<1000;++page) {
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
    if(result.id.isEmpty() || result.title.isEmpty())throw ProviderError("Metadata provider returned an invalid title record.");
    result.poster=imageUrl(result.poster);result.logo=imageUrl(result.logo);return result;
}

MetadataClient::Artwork MetadataClient::artwork(const QString &source,const Record &record,const ShowDetail &show,
    const QString &order,const QVariantMap &credentials,const Cancellation &cancel) const
{
    return artworkFor(source, record, {show.show.title, show.show.year, record.ids, {}}, order, credentials, cancel);
}

MetadataClient::Artwork MetadataClient::movieArtwork(const QString &source, const Record &record, const MovieSummary &movie,
    const QVariantMap &credentials, const Cancellation &cancel) const
{
    return artworkFor(source, record, {movie.title, movie.year, record.ids, {}, true}, "official", credentials, cancel);
}

MetadataClient::Artwork MetadataClient::artworkFor(const QString &source, const Record &record, const Title &title,
    const QString &order, const QVariantMap &credentials, const Cancellation &cancel) const
{
    if(source=="off")return {};
    if(source!="fanart") {
        const auto art=record.source==source?record:identifyTitle(source,title,{},source=="tvdb"?order:QString("official"),credentials,cancel,true);
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
        return {art.poster,art.logo,art.artworkError};
    }
    const auto key=credentials.value("fanart").toString().trimmed();
    if(key.isEmpty())throw ProviderError("Save a fanart.tv personal API key in Settings → Media.");
    QString target,origin;
    if(!title.movie && record.ids.tvdb) {target=*record.ids.tvdb;origin="tvdb";}
    else if(record.ids.tmdb) {target=*record.ids.tmdb;origin="tmdb";}
    else if(record.ids.imdb) {target=*record.ids.imdb;origin="imdb";}
    else throw ProviderError("This title has no identifier supported by fanart.tv.");
    QUrl url(m_bases.value("fanart").toString()+(title.movie ? "/movies/" : "/tv/")+QUrl::toPercentEncoding(target));
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
    if (title.movie) return {pick({"movieposter"}), pick({"hdmovielogo", "movielogo"})};
    return {pick({"tvposter"}),pick({"hdtvlogo","clearlogo"})};
}
} // namespace introvert::providers
