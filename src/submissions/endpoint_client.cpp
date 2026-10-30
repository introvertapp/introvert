#include "endpoint_client.h"
#include "providers/http_transport.h"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QTimeZone>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <limits>

namespace introvert::submissions {
namespace {
const QStringList kinds{"recap", "intro", "credits", "preview"};

bool null(const QVariant &value) { return !value.isValid() || value.isNull(); }
bool truthy(const QVariant &value)
{
    if (null(value)) return false;
    if (value.metaType().id() == QMetaType::QString) return !value.toString().isEmpty();
    if (value.metaType().id() == QMetaType::QVariantList) return !value.toList().isEmpty();
    if (value.metaType().id() == QMetaType::QVariantMap) return !value.toMap().isEmpty();
    return value.toDouble() != 0;
}
std::optional<double> numeric(const QVariant &value)
{
    if (null(value) || value.metaType().id() == QMetaType::Bool) return {};
    double result;
    if (value.metaType().id() == QMetaType::QString) {
        QString text;
        for (const auto code : value.toString().trimmed().toUcs4()) {
            if (QChar::category(code) == QChar::Number_DecimalDigit) text.append(QChar('0' + QChar::digitValue(code)));
            else { const char32_t character = code; text.append(QString::fromUcs4(&character, 1)); }
        }
        static const QRegularExpression syntax(QStringLiteral(
            "\\A[+-]?(?:[0-9](?:_?[0-9])*(?:\\.(?:[0-9](?:_?[0-9])*)?)?|\\.[0-9](?:_?[0-9])*)(?:[eE][+-]?[0-9](?:_?[0-9])*)?\\z"));
        if (!syntax.match(text).hasMatch()) return {};
        text.remove('_'); result = text.toDouble();
    } else {
        const auto type = value.metaType().id();
        if (type != QMetaType::Int && type != QMetaType::LongLong && type != QMetaType::UInt
            && type != QMetaType::ULongLong && type != QMetaType::Double && type != QMetaType::Float) return {};
        result = value.toDouble();
    }
    return std::isfinite(result) ? std::optional<double>(result) : std::nullopt;
}
std::optional<double> positive(const QVariant &value)
{
    const auto n = numeric(value);
    return n && *n > 0 && std::floor(*n) == *n ? n : std::nullopt;
}
bool integralType(const QVariant &value)
{
    const auto type = value.metaType().id();
    return type == QMetaType::Int || type == QMetaType::LongLong || type == QMetaType::UInt || type == QMetaType::ULongLong;
}
bool integer(const QVariant &value, double minimum = 0, double maximum = std::numeric_limits<double>::infinity())
{
    if (!integralType(value) && value.metaType().id() != QMetaType::Double && value.metaType().id() != QMetaType::Float) return false;
    const auto n = numeric(value);
    return n && std::floor(*n) == *n && *n >= minimum && *n <= maximum;
}
QString string(const QVariant &value)
{
    if (null(value)) return "None";
    if (value.metaType().id() == QMetaType::Bool) return value.toBool() ? "True" : "False";
    if (value.metaType().id() == QMetaType::QVariantMap || value.metaType().id() == QMetaType::QVariantList)
        return QString::fromUtf8(QJsonValue::fromVariant(value).toJson(QJsonValue::JsonFormat::Compact));
    return value.toString();
}
QString base(QString value) { while (value.endsWith('/')) value.chop(1); return value; }
double rounded(double value) { return std::floor(value + .5); }
QVariant first(const QVariantMap &map, const QStringList &names)
{
    for (const auto &name : names) if (map.contains(name)) return map.value(name);
    return {};
}
bool imdb(const QString &value)
{
    static const QRegularExpression pattern(QStringLiteral("\\Att\\d{7,8}\\z"), QRegularExpression::UseUnicodePropertiesOption);
    return pattern.match(value).hasMatch();
}
bool mediaBody(const QJsonObject &body)
{
    const auto row = body.toVariantMap();
    if (!integer(row.value("tvdb_id"), 1, 100000000) || !QStringList{"tv", "movie"}.contains(row.value("type").toString())) return false;
    for (const auto &key : {"season", "episode"}) if (!null(row.value(key)) && !integer(row.value(key), 0)) return false;
    for (const auto &kind : kinds) {
        if (!body.contains(kind)) continue;
        if (!body[kind].isArray()) return false;
        for (const auto &value : body[kind].toArray()) {
            if (!value.isObject()) return false;
            const auto match = value.toObject().toVariantMap();
            if (!match.contains("start_ms") || !match.contains("end_ms")) return false;
            for (const auto &key : {"start_ms", "end_ms", "submission_count"})
                if (!null(match.value(key)) && !integer(match.value(key))) return false;
            const auto confidence = match.value("confidence");
            if (!null(confidence) && ((!integralType(confidence) && confidence.metaType().id() != QMetaType::Double)
                || !numeric(confidence))) return false;
        }
    }
    return true;
}
bool submissionBody(const QJsonObject &body)
{
    if (body["ok"] != true || !body["submissions"].isArray()) return false;
    static const QRegularExpression uuid(QStringLiteral("\\A[0-9a-fA-F]{8}-(?:[0-9a-fA-F]{4}-){3}[0-9a-fA-F]{12}\\z"));
    for (const auto &value : body["submissions"].toArray()) {
        if (!value.isObject()) return false;
        const auto row = value.toObject().toVariantMap();
        if (row.value("id").metaType().id() != QMetaType::QString || !uuid.match(row.value("id").toString()).hasMatch()
            || !row.contains("tvdbId") || !(null(row.value("tvdbId")) || integer(row.value("tvdbId"), 1, 100000000)) || !QStringList{"tv", "movie"}.contains(row.value("type").toString())
            || !kinds.contains(row.value("segment").toString())) return false;
        for (const auto &key : {"season", "episode"}) if (!null(row.value(key)) && !integer(row.value(key), 0)) return false;
        if (!row.contains("videoDurationMs") || (!null(row.value("videoDurationMs")) && !integer(row.value("videoDurationMs"), 0, 21600000))) return false;
        for (const auto &key : {"startMs", "endMs"}) if (!null(row.value(key)) && !integer(row.value(key))) return false;
        if (!QStringList{"pending", "accepted", "rejected"}.contains(row.value("status").toString())) return false;
        const auto weight = row.value("weight"); const auto n = numeric(weight);
        if ((!integralType(weight) && weight.metaType().id() != QMetaType::Double) || !n || *n < 0) return false;
    }
    return true;
}
std::optional<QString> iso(double seconds)
{
    // Supported dates use years 1..9999. Round to microseconds before
    // truncating to the millisecond output precision.
    if (!std::isfinite(seconds) || seconds < -62135596800. || seconds >= 253402300800.) return {};
    const auto milliseconds = std::floor(std::nearbyint(seconds * 1000000) / 1000);
    const auto date = QDateTime::fromMSecsSinceEpoch(qint64(milliseconds), QTimeZone::UTC);
    return date.isValid() ? std::optional<QString>(date.toString(Qt::ISODateWithMs)) : std::nullopt;
}
}

EndpointClient::EndpointClient(QString endpoint, QVariantMap settings, std::optional<QString> key, Clock clock)
    : m_endpoint(std::move(endpoint)), m_settings(std::move(settings)), m_key(std::move(key)), m_clock(std::move(clock))
{
    if (m_endpoint != "introdb_app" && m_endpoint != "theintrodb_org") throw SubmissionError("Unsupported submission endpoint");
    SettingsStore::validateEndpoints(m_settings);
    if (!m_clock) m_clock = [] { return QDateTime::currentMSecsSinceEpoch() / 1000.; };
}
EndpointClient EndpointClient::fromStore(const QString &endpoint, const SettingsStore &store, Clock clock)
{
    if (endpoint != "introdb_app" && endpoint != "theintrodb_org") throw SubmissionError("Unsupported submission endpoint");
    return EndpointClient(endpoint, store.loadEndpoints(), store.credential("endpoints:" + endpoint), std::move(clock));
}
bool EndpointClient::enabled() const
{
    return m_settings.value(m_endpoint == "introdb_app" ? "introdb_enabled" : "theintrodb_enabled").toBool();
}

std::optional<QString> EndpointClient::resetDate(const QVariant &value, double now, bool relative)
{
    if (null(value) || value == QVariant(QString())) return {};
    if (const auto n = numeric(value)) return iso(relative ? now + std::max(0., *n) : *n > 100000000000. ? *n / 1000 : *n);
    auto text = string(value);
    // Qt's RFC parser accepts numeric offsets but not the named zones accepted
    // by email.utils.parsedate_to_datetime in the reference.
    static const QRegularExpression zone(QStringLiteral("\\s([A-Za-z]+)\\z"));
    const auto match = zone.match(text);
    if (match.hasMatch()) {
        static const QMap<QString, QString> zones{{"AST", "-0400"}, {"ADT", "-0300"}, {"EST", "-0500"}, {"EDT", "-0400"},
            {"CST", "-0600"}, {"CDT", "-0500"}, {"MST", "-0700"}, {"MDT", "-0600"}, {"PST", "-0800"}, {"PDT", "-0700"}};
        text.replace(match.capturedStart(1), match.capturedLength(1), zones.value(match.captured(1).toUpper(), "+0000"));
    }
    auto date = QDateTime::fromString(text, Qt::RFC2822Date);
    if (!date.isValid()) date = QDateTime::fromString(string(value), Qt::ISODateWithMs);
    if (!date.isValid()) date = QDateTime::fromString(string(value), Qt::ISODate);
    if (!date.isValid()) return {};
    if (date.timeSpec() == Qt::LocalTime) date.setTimeZone(QTimeZone::UTC);
    return iso(date.toMSecsSinceEpoch() / 1000.);
}

EndpointClient::Response EndpointClient::request(QString url, const QVariantMap &query, const std::optional<QJsonObject> &body,
                                                bool authenticated, const providers::Cancellation &cancel) const
{
    if (cancel && cancel->load()) throw EndpointError("Endpoint request cancelled");
    QByteArrayList encoded;
    for (auto field = query.cbegin(); field != query.cend(); ++field) {
        const auto quote = [](const QString &text) { return QUrl::toPercentEncoding(text).replace("%20", "+"); };
        encoded.append(quote(field.key()) + '=' + quote(string(field.value())));
    }
    if (!encoded.isEmpty()) url += '?' + QString::fromLatin1(encoded.join('&'));
    QMap<QByteArray, QByteArray> headers{{"Accept", "application/json"}, {"User-Agent","Introvert"}};
    if (authenticated && m_key && !m_key->isEmpty())
        headers[m_endpoint == "introdb_app" ? "X-API-Key" : "Authorization"]
            = (m_endpoint == "introdb_app" ? *m_key : "Bearer " + m_key->trimmed()).toUtf8();
    std::optional<QByteArray> bytes;
    if (body) { headers["Content-Type"] = "application/json"; bytes = QJsonValue(*body).toJson(QJsonValue::JsonFormat::Compact); }
    try {
        const auto response = providers::detail::exchange(QUrl(url, QUrl::StrictMode), headers,
            m_settings.value("api_timeout_ms").toInt() / 1000., 2 * 1024 * 1024, cancel, bytes, true);
        QJsonParseError error;
        const auto parsed = QJsonValue::fromJson(response.data, &error);
        return {response.status, response.headers, error.error == QJsonParseError::NoError ? parsed : QJsonValue()};
    } catch (const providers::Cancelled &) { throw EndpointError("Endpoint request cancelled"); }
    catch (const providers::detail::HttpFailure &error) {
        if (error.kind == providers::detail::HttpFailureKind::TooLarge) throw EndpointError("Endpoint returned an oversized response");
        throw EndpointError("Endpoint request failed. Check the URL, network, timeout and server certificate.");
    }
}

LookupResult EndpointClient::lookup(const QVariantMap &values, const providers::Cancellation &cancel) const
{
    const auto kind = values.value("segment_type").toString();
    if (!enabled() || !kinds.contains(kind)) return {};
    const bool intro = m_endpoint == "introdb_app";
    QVariantMap query; QString segment = kind, url;
    if (intro) {
        if (!truthy(values.value("imdb_id"))) return {};
        segment = kind == "credits" ? "outro" : kind;
        query = {{"imdb_id", values.value("imdb_id")}, {"season", values.value("season")}, {"episode", values.value("episode")}, {"segment_type", segment}};
        url = m_settings.value("introdb_lookup_url").toString();
    } else {
        if (!integer(values.value("season")) || !integer(values.value("episode"))) return {};
        try {
            auto identity=values;identity["start_sec"]=0.;identity["end_sec"]=6.;
            const auto outgoing = payload(identity,false).body;
            for (const auto &name : {"tvdb_id", "tmdb_id", "imdb_id", "mapping", "tvdb_episode_id", "episode_name"})
                if (outgoing.contains(name)) query[name] = outgoing[name].toVariant();
            query["season"] = outgoing["season"].toVariant(); query["episode"] = outgoing["episode"].toVariant();
            if (outgoing.contains("video_duration_ms")) query["duration_ms"] = outgoing["video_duration_ms"].toVariant();
            query["details"] = "true";
        } catch (const SubmissionError &) { return {}; }
        url = base(m_settings.value("theintrodb_base_url").toString()) + "/media";
    }
    try {
        const auto response = request(url, query, {}, !intro, cancel);
        if(!intro && response.status==429) {LookupResult result;result.failure=rateLimit(response);return result;}
        if(!intro && response.status!=200 && response.status!=404) {
            LookupResult result;result.failure=SubmissionResult{"FAIL",response.status,"Cannot verify existing segments. Check the endpoint API key or retry later.",{}, {}};return result;
        }
        if (!intro && response.status==200 && (!response.body.isObject() || !mediaBody(response.body.toObject()))) {
            LookupResult result;result.failure=SubmissionResult{"FAIL",response.status,"Cannot verify existing segments: the endpoint returned an invalid response.",{}, {}};return result;
        }
        if (response.status != 200 || !response.body.isObject()) return {};
        const auto body = response.body.toObject().toVariantMap();
        auto value = body.value(segment);
        if (intro && !truthy(value)) value = body.value(kind);
        if (intro && !truthy(value)) value = body.value("segment");
        const auto rows = value.metaType().id() == QMetaType::QVariantList ? value.toList() : QVariantList{value};
        LookupResult result;
        for (const auto &value : rows) {
            if (value.metaType().id() != QMetaType::QVariantMap) continue;
            const auto row = value.toMap();
            auto start = intro ? numeric(first(row, {"start_sec", "startSec", "start"})) : null(row.value("start_ms"))
                ? std::optional<double>(0.) : numeric(row.value("start_ms"));
            const auto rawEnd = intro ? first(row, {"end_sec", "endSec", "end"}) : row.value("end_ms");
            auto end = numeric(rawEnd);
            if (!intro) { if (start) start = *start / 1000; if (end) end = *end / 1000; }
            if (!start || (!null(rawEnd) && !end)) continue;
            result.matches.append({*start, end, row.value("confidence"), first(row, {"submission_count", "submissionCount"}),
                                   intro ? first(row, {"updated_at", "updatedAt"}) : QVariant()});
        }
        return result;
    } catch (const EndpointError &error) {
        LookupResult result;
        if(!intro)result.failure=SubmissionResult{"FAIL",{},QString::fromUtf8(error.what()),{}, {}};
        return result;
    }
}

EndpointClient::Payload EndpointClient::payload(const QVariantMap &values, bool requireCredential) const
{
    if (requireCredential && !configured()) throw SubmissionError("Enable the endpoint and save its API key in Settings");
    const auto start = numeric(values.value("start_sec")), end = numeric(values.value("end_sec"));
    const auto kind = values.value("segment_type").toString(), imdbId = values.value("imdb_id").toString();
    if (!kinds.contains(kind) || !start || *start < 0 || (!null(values.value("end_sec")) && (!end || *end <= *start)))
        throw SubmissionError("Invalid segment timestamps");
    const auto season = values.value("season"), episode = values.value("episode");
    if (!integralType(season) || !integralType(episode) || !integer(season) || !integer(episode))
        throw SubmissionError("Invalid season or episode number");
    if (m_endpoint == "introdb_app") {
        if (!truthy(values.value("imdb_id"))) throw SubmissionError("Missing imdbId");
        return {m_settings.value("introdb_api_url").toString(), QJsonObject::fromVariantMap({
            {"imdb_id", values.value("imdb_id")}, {"season", season}, {"episode", episode},
            {"segment_type", kind == "credits" ? QString("outro") : kind}, {"start_sec", *start}, {"end_sec", end ? QVariant(*end) : QVariant()}})};
    }
    QVariantMap body{{"type", "tv"}, {"segment", kind},
        {"season", values.value("target_season", season)}, {"episode", values.value("target_episode", episode)},
        {"start_ms", (kind == "intro" || kind == "recap") && *start == 0 ? QVariant() : QVariant(rounded(*start * 1000))},
        {"end_ms", end ? QVariant(rounded(*end * 1000)) : QVariant()}};
    const auto tvdb = positive(values.value("tvdb_id")), tmdb = positive(values.value("tmdb_id"));
    const auto source = values.value("numbering_source").toString();
    const auto exact = positive(values.value("tvdb_episode_id"));
    if (tvdb && *tvdb <= 100000000 && (source == "tvdb" || exact || (!tmdb && source != "imdb"))) body["tvdb_id"] = *tvdb;
    else if (tmdb && *tmdb <= 10000000 && source != "imdb") body["tmdb_id"] = *tmdb;
    else if (imdb(imdbId)) body["imdb_id"] = imdbId;
    else throw SubmissionError("TheIntroDB requires a TVDB, TMDB or IMDb ID compatible with the episode numbering");
    const auto mapping = values.value("mapping").toString();
    if (!mapping.isEmpty()) {
        if (!QStringList{"default","official","dvd","alternate","absolute","regional"}.contains(mapping))
            throw SubmissionError("Unknown episode order. Choose a supported episode order before submitting.");
        body["mapping"] = mapping;
    }
    if (exact) { body["tvdb_episode_id"] = *exact; body.remove("mapping"); }
    if (!values.value("episode_name").toString().isEmpty()) body["episode_name"] = values.value("episode_name");
    if (!integer(body["season"],0,1000) || !integer(body["episode"],0,10000)) throw SubmissionError("Invalid season or episode number");
    const auto duration = numeric(values.value("video_duration_ms"));
    if (duration && (*duration == 0 || (*duration >= 300000 && *duration <= 21600000)) && std::floor(*duration) == *duration)
        body["video_duration_ms"] = *duration;
    if (*start > 21600 || (end && *end > 21600) || ((kind == "intro" || kind == "recap") && !end))
        throw SubmissionError("Invalid TheIntroDB segment timestamps");
    if (end) {
        const auto length = *end - *start;
        const auto maximum = kind == "intro" ? 300 : kind == "recap" ? 1200 : 1800;
        if (length < 5 || length > maximum) throw SubmissionError("Segment duration is outside TheIntroDB limits");
    }

    return {base(m_settings.value("theintrodb_base_url").toString()) + "/submit", QJsonObject::fromVariantMap(body)};
}

QVariantMap EndpointClient::resolve(const QVariantMap &values, const providers::Cancellation &cancel) const
{
    if (m_endpoint != "theintrodb_org" || (positive(values.value("tvdb_episode_id")) && positive(values.value("tvdb_id")))) return values;
    auto body = payload(values).body;
    for (const auto &name : {"segment","start_ms","end_ms","video_duration_ms"}) body.remove(name);
    Response response;
    try { response = request(base(m_settings.value("theintrodb_base_url").toString()) + "/map", {}, body, true, cancel); }
    catch (const EndpointError &) { throw SubmissionError("Cannot verify the submission episode. Check network access and retry."); }
    const auto row = response.body.toObject();
    if (response.status != 200 || !integer(row["tvdb_id"].toVariant(),1,100000000)
        || row["type"] != "tv" || !integer(row["season"].toVariant(),0,1000) || !integer(row["episode"].toVariant(),0,10000))
        throw SubmissionError(response.status == 429 ? "Episode verification is rate limited. Wait before trying again."
            : "Cannot verify the episode mapping. Check the source IDs, episode order and endpoint API key.");
    auto result = values;
    if(positive(values.value("tvdb_id")) && values.value("tvdb_id").toLongLong()!=row["tvdb_id"].toInteger())
        throw SubmissionError("The resolved TVDB show conflicts with the saved identifiers. Correct the show identification before submitting.");
    result["tvdb_id"] = row["tvdb_id"].toVariant(); result["numbering_source"] = "tvdb";
    result["target_season"] = row["season"].toVariant(); result["target_episode"] = row["episode"].toVariant();
    result["mapping"] = "default";
    if (positive(row["tvdb_episode_id"].toVariant())) result["tvdb_episode_id"] = row["tvdb_episode_id"].toVariant();
    result["resolved_title"] = row["episode_title"].toString();
    return result;
}

SubmissionResult EndpointClient::rateLimit(const Response &response) const
{
    const auto body=response.body.toObject().toVariantMap();
    const bool intro=m_endpoint=="introdb_app";
        std::optional<QString> reset, resume;
        if (intro) {
            QVariant raw;
            for (const auto &value : {body.value("reset_at"), body.value("resetAt"), QVariant(QString::fromUtf8(response.headers.value("x-ratelimit-reset"))),
                                       QVariant(QString::fromUtf8(response.headers.value("x-rate-limit-reset")))})
                if (truthy(value)) { raw = value; break; }
            reset = resetDate(raw, m_clock());
            if (!reset) reset = resetDate(QString::fromUtf8(response.headers.value("retry-after")), m_clock(), true);
            resume = reset ? reset : iso(m_clock() + 3600);
        } else {
            std::optional<double> seconds;
            for(const auto &name:{"x-ratelimit-reset","x-usagelimit-reset","x-usagelimit-specificmedia-reset","retry-after"}) {
                const auto value=numeric(QString::fromUtf8(response.headers.value(name)));
                if(value && *value>0)seconds=std::max(seconds.value_or(0.),*value);
            }
            resume = iso(m_clock() + (seconds && *seconds > 0 ? *seconds : 3600)); reset = resume;
        }
        return {"RATE LIMITED", response.status, "Rate limit exceeded", reset, resume};
}

void EndpointClient::validate(const QVariantMap &values) const { payload(values); }
SubmissionResult EndpointClient::submit(const QVariantMap &values, const providers::Cancellation &cancel) const
{
    Payload outgoing;
    try { outgoing = payload(values); }
    catch (const SubmissionError &error) { return {"FAIL", 400, QString::fromUtf8(error.what()), {}, {}}; }
    Response response;
    try { response = request(outgoing.url, {}, outgoing.body, true, cancel); }
    catch (const EndpointError &error) { return {"FAIL", {}, QString::fromUtf8(error.what()), {}, {}}; }
    const auto body = response.body.isObject() ? response.body.toObject().toVariantMap() : QVariantMap();
    const bool intro = m_endpoint == "introdb_app";
    if (response.status == 429 || (intro && response.status >= 400 && string(body.value("error")).contains("Rate limit exceeded"))) {
        return rateLimit(response);
    }
    const bool success = intro ? response.status == 200 : response.status >= 200 && response.status < 300 && submissionBody(response.body.toObject());
    return {success ? "OK" : "FAIL", response.status, success ? QString() : QString("Endpoint rejected the submission or returned an invalid response"), {}, {}};
}

QString comparison(const LookupResult &lookup, double start, const std::optional<double> &end)
{
    for (const auto &match : lookup.matches) if (match.startSeconds == start && match.endSeconds == end) return "DUPLICATE";
    return lookup.found() ? QString("DIFFERENT") : QString("MISSING");
}

} // namespace introvert::submissions
