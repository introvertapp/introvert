#include "range_stream.h"
#include "providers/plex_provider.h"
#include "providers/emby_provider.h"
#include "providers/json_values.h"
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QSet>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>
#include <mutex>

namespace introvert::editor {
using namespace providers;
namespace {

struct Source {
    QUrl url;
    QMap<QByteArray, QByteArray> headers;
    int timeoutMs;
    int resumeAttempts=0;
    std::shared_ptr<std::atomic_int> resumed;
};

bool truthy(const QJsonValue &value)
{
    if (value.isUndefined() || value.isNull()) return false;
    if (value.isBool()) return value.toBool();
    if (value.isString()) return !value.toString().isEmpty();
    if (value.isArray()) return !value.toArray().isEmpty();
    if (value.isObject()) return !value.toObject().isEmpty();
    return value.toDouble() != 0;
}

QByteArray jsonToken(const QString &token)
{
    const auto quoted = QJsonValue(token).toJson(QJsonValue::JsonFormat::Compact);
    QByteArray result;
    for (const auto c : QString::fromUtf8(quoted))
        if (c.unicode() > 126) result.append("\\u" + QByteArray::number(c.unicode(), 16).rightJustified(4, '0'));
        else result.append(char(c.unicode()));
    return result;
}

QByteArray randomBytes(qsizetype count)
{
    QByteArray bytes(count, Qt::Uninitialized);
    for (auto &byte : bytes) byte = char(QRandomGenerator::system()->generate() & 255);
    return bytes;
}

Source resolve(const ProviderConfig &config, const QString &episode, const Cancellation &cancel)
{
    checkCancelled(cancel);
    if (config.provider == ProviderId::Local) throw ProviderError("Local media uses its existing file path.");
    if (!std::isfinite(config.timeoutSeconds) || config.timeoutSeconds <= 0)
        throw ProviderError("Media request timeout must be a finite positive number of seconds.");
    const int timeout = int(std::clamp(config.timeoutSeconds * 1000, 1., 10000.));
    const auto base = detail::baseUrl(config.remoteUrl.value_or(QString()));
    const auto id = detail::quote(episode);
    if (config.provider == ProviderId::Plex) {
        PlexProvider provider;
        const auto metadata = provider.request(config, "/library/metadata/" + id, {}, cancel).toObject();
        QJsonObject item;
        for (const auto &value : metadata["Metadata"].toArray()) {
            if (detail::sourceString(value.toObject()["ratingKey"]) == episode) { item = value.toObject(); break; }
        }
        const auto sources = item["Media"].toArray();
        const auto parts = sources.isEmpty() ? QJsonArray() : sources.first().toObject()["Part"].toArray();
        if (parts.size() != 1) throw ProviderError("Streaming this Plex episode requires a single media part.");
        const auto key = parts.first().toObject()["key"].toString();
        const QUrl path(key, QUrl::StrictMode);
        bool controls = false;
        for (const auto c : key) if (c.unicode() < 32) controls = true;
        if (!key.startsWith("/library/parts/") || !path.isValid() || !path.authority().isEmpty()
            || !path.scheme().isEmpty() || path.hasQuery() || path.hasFragment() || key.contains('\\') || controls)
            throw ProviderError("Plex did not return a supported media stream.");
        return {QUrl::fromEncoded((base + key).toUtf8(), QUrl::StrictMode),
                {{"X-Plex-Token", config.credential->toUtf8()}, {"X-Plex-Product", "Introvert"}}, timeout};
    }
    const bool jellyfin = config.provider == ProviderId::Jellyfin;
    const QString label = jellyfin ? "Jellyfin" : "Emby", prefix = jellyfin ? "" : "/emby";
    std::unique_ptr<MediaProvider> provider = jellyfin ? std::unique_ptr<MediaProvider>(new JellyfinProvider)
                                                     : std::unique_ptr<MediaProvider>(new EmbyProvider);
    const auto users = provider->request(config, prefix + "/Users", {}, cancel);
    if (!users.isArray()) throw ProviderError(label + " returned an invalid user list.");
    const auto values = users.toArray();
    if (jellyfin && !values.isEmpty() && !values.first().isObject()) throw ProviderError(label + " returned an invalid user list.");
    const auto user = values.isEmpty() ? QJsonValue() : values.first().toObject()["Id"];
    if (!truthy(user)) throw ProviderError("No " + label + " user is available for media access.");
    const auto info = provider->request(config, prefix + "/Items/" + id + "/PlaybackInfo",
                                         {{"UserId", detail::sourceString(user)}}, cancel);
    const auto object = info.toObject();
    const auto sources = object["MediaSources"].toArray();
    if (!info.isObject() || truthy(object["ErrorCode"]) || sources.isEmpty())
        throw ProviderError(label + " did not return an accessible media source.");
    const auto source = sources.first().toObject();
    if (!sources.first().isObject() || !truthy(source["Id"]) || truthy(source["RequiresOpening"])
        || truthy(source["IsInfiniteStream"]) || detail::sourceString(source.contains("Protocol") ? source["Protocol"] : QJsonValue("File")).toLower() != "file"
        || QStringList{"dvd", "bluray"}.contains(detail::sourceString(source.contains("VideoType") ? source["VideoType"] : QJsonValue("VideoFile")).toLower()))
        throw ProviderError(label + " streaming currently requires a regular media file.");
    const auto session = truthy(object["PlaySessionId"]) ? detail::sourceString(object["PlaySessionId"])
                                                       : QString::fromLatin1(randomBytes(16).toHex());
    const auto quotePlus = [](const QString &value) { return QUrl::toPercentEncoding(value).replace("%20", "+"); };
    const auto query = QByteArray("Static=true&MediaSourceId=") + quotePlus(detail::sourceString(source["Id"]))
        + "&PlaySessionId=" + quotePlus(session);
    const auto url = (base + prefix + "/Videos/" + id + "/stream?").toUtf8() + query;
    return {QUrl::fromEncoded(url, QUrl::StrictMode),
        jellyfin ? QMap<QByteArray, QByteArray>{{"Authorization", "MediaBrowser Token=" + jsonToken(*config.credential)}}
                 : QMap<QByteArray, QByteArray>{{"X-Emby-Token", config.credential->toUtf8()}}, timeout};
}

constexpr qint64 chunkBytes = 64 * 1024, outputBytes = 128 * 1024;

class Connection final : public QObject {
public:
    Connection(QTcpSocket *socket, QNetworkAccessManager &manager, const Source &source, QByteArray route, QObject *parent)
        : QObject(parent), m_socket(socket), m_manager(manager), m_source(source), m_route(std::move(route))
    {
        socket->setParent(this); socket->setReadBufferSize(chunkBytes);
        m_timer.setSingleShot(true); m_timer.setInterval(source.timeoutMs);
        connect(&m_timer, &QTimer::timeout, this, [this] { error(m_started ? 502 : 408); });
        connect(socket, &QTcpSocket::readyRead, this, [this] { readRequest(); });
        connect(socket, &QTcpSocket::bytesWritten, this, [this] { pump(); });
        connect(socket, &QTcpSocket::disconnected, this, [this] {
            m_closing = true;
            if (m_reply && !m_reply->isFinished()) m_reply->abort();
            deleteLater();
        });
        m_timer.start();
        if (socket->bytesAvailable()) readRequest();
    }
    ~Connection() override
    {
        m_closing = true;
        if (m_reply) { m_reply->disconnect(this); m_reply->abort(); delete m_reply; }
        m_socket->disconnect(this); m_socket->abort();
    }
private:
    void close()
    {
        if (m_closing) return;
        m_closing = true; m_timer.stop();
        if (m_reply && !m_reply->isFinished()) m_reply->abort();
        m_socket->disconnectFromHost();
    }
    void error(int status, const QByteArray &range = {})
    {
        if (m_closing) return;
        if (!m_sent) {
            auto wire = "HTTP/1.1 " + QByteArray::number(status) + " Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n";
            if (status == 416 && !range.contains('\r') && !range.contains('\n') && !range.isEmpty()) wire += "Content-Range: " + range + "\r\n";
            m_socket->write(wire + "\r\n"); m_sent = true;
        }
        close();
    }
    void readRequest()
    {
        if (m_started || m_closing) return;
        m_request.append(m_socket->read(chunkBytes + 1 - m_request.size()));
        const auto end = m_request.indexOf("\r\n\r\n");
        if (m_request.size() > chunkBytes) { error(431); return; }
        if (end < 0) return;
        m_timer.stop(); m_started = true;
        const auto lines = m_request.left(end).split('\n');
        const auto first = lines.first().trimmed().split(' ');
        if (first.size() != 3 || (first[2] != "HTTP/1.1" && first[2] != "HTTP/1.0")) { error(400); return; }
        if (first[0] != "GET" && first[0] != "HEAD") { error(501); return; }
        if (first[1] != m_route) { error(404); return; }
        m_head = first[0] == "HEAD";
        QNetworkRequest request(m_source.url);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
        request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
        request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
        request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
        request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
        request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
        // The reference times out an upstream read, not a blocked downstream
        // write. A QNetworkRequest transfer timeout also counts intentional
        // backpressure, so the timer below runs only while awaiting input.
        request.setTransferTimeout(std::chrono::milliseconds(0));
        for (auto it = m_source.headers.cbegin(); it != m_source.headers.cend(); ++it) request.setRawHeader(it.key(), it.value());
        request.setRawHeader("Accept-Encoding", "identity");
        QSet<QByteArray> forwarded;
        for (const auto &line : lines.mid(1)) {
            const auto colon = line.indexOf(':');
            if (colon < 1) { error(400); return; }
            const auto name = line.left(colon).trimmed().toLower(), value = line.mid(colon + 1).trimmed();
            if ((name == "range" || name == "if-range") && !forwarded.contains(name)) {
                if (value.contains('\r') || value.contains('\n')) { error(400); return; }
                request.setRawHeader(name, value); forwarded.insert(name);
            }
        }
        m_request.clear();
        m_upstream=request; startRequest();
    }
    void startRequest()
    {
        m_headersChecked=false;
        m_reply = m_head ? m_manager.head(m_upstream) : m_manager.get(m_upstream);
        m_reply->setParent(this); m_reply->setReadBufferSize(chunkBytes);
        connect(m_reply, &QNetworkReply::metaDataChanged, this, [this] { pump(); });
        connect(m_reply, &QIODevice::readyRead, this, [this] { m_timer.stop(); pump(); });
        connect(m_reply, &QNetworkReply::finished, this, [this] { pump(); });
        m_timer.start();
        pump();
    }
    bool resume()
    {
        if(m_head || m_closing || m_resumes>=m_source.resumeAttempts)return false;
        if(m_sent && (m_end<0 || m_bodyQueued>m_end-m_start))return false;
        if(m_sent) {
            m_upstream.setRawHeader("Range","bytes="+QByteArray::number(m_start+m_bodyQueued)+'-'+QByteArray::number(m_end));
            if(!m_ifRange.isEmpty())m_upstream.setRawHeader("If-Range",m_ifRange);
        }
        ++m_resumes; ++*m_source.resumed; m_timer.stop();
        m_reply->disconnect(this); if(!m_reply->isFinished())m_reply->abort();m_reply->deleteLater();m_reply=nullptr;
        QTimer::singleShot(25*std::min(m_resumes,8),this,[this]{if(!m_closing)startRequest();});
        return true;
    }
    bool validateHeaders(int code)
    {
        if(m_source.resumeAttempts==0 || m_head)return true;
        const auto header=m_reply->rawHeader("Content-Range");
        static const QRegularExpression range(QStringLiteral("^bytes ([0-9]+)-([0-9]+)/([0-9]+)$"));
        const auto match=range.match(QString::fromLatin1(header));
        bool sizeOk=false; const auto length=m_reply->header(QNetworkRequest::ContentLengthHeader).toLongLong(&sizeOk);
        qint64 start=0,end=-1,total=-1;
        if(code==206) {
            if(!match.hasMatch())return false;
            bool a=false,b=false,c=false;start=match.captured(1).toLongLong(&a);end=match.captured(2).toLongLong(&b);total=match.captured(3).toLongLong(&c);
            if(!a || !b || !c || start<0 || end<start || total<=end || (sizeOk && length!=end-start+1))return false;
        } else if(code==200 && sizeOk && length>0) {end=length-1;total=length;}
        else if(m_sent)return false;
        const auto etag=m_reply->rawHeader("ETag"),modified=m_reply->rawHeader("Last-Modified");
        const auto validator=etag.isEmpty()?modified:etag;
        if(validator.contains('\r') || validator.contains('\n') || modified.contains('\r') || modified.contains('\n'))return false;
        if(m_sent) {
            if(code!=206 || start!=m_start+m_bodyQueued || end!=m_end || total!=m_total || (!m_validator.isEmpty() && validator!=m_validator))return false;
        } else {
            const auto wanted=QRegularExpression(QStringLiteral("^bytes=([0-9]+)-([0-9]*)$")).match(QString::fromLatin1(m_upstream.rawHeader("Range")));
            if(code==206 && wanted.hasMatch() && (start!=wanted.captured(1).toLongLong() || (!wanted.captured(2).isEmpty() && end!=std::min(total-1,wanted.captured(2).toLongLong()))))return false;
            m_start=start;m_end=end;m_total=total;m_validator=validator;
            m_ifRange=!etag.isEmpty() && !etag.startsWith("W/")?etag:modified;
        }
        return true;
    }
    void pump()
    {
        if (m_closing || !m_reply) return;
        if (!m_headersChecked) {
            const auto status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
            if (!status.isValid()) {
                if (m_reply->isFinished() && !resume()) error(502);
                return;
            }
            const auto code = status.toInt();
            if (code < 200 || code >= 300) {
                if((code==429 || code==500 || code==502 || code==503 || code==504) && resume())return;
                error(code == 416 ? 416 : 502, m_reply->rawHeader("Content-Range"));return;
            }
            if(!validateHeaders(code)){error(502);return;}
            m_headersChecked=true;
        }
        if (!m_sent) {
            const auto code=m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            m_timer.stop();
            auto wire = "HTTP/1.1 " + QByteArray::number(code) + " Media\r\n";
            for (const auto &name : {"Content-Type", "Content-Length", "Content-Range", "Accept-Ranges", "ETag", "Last-Modified"}) {
                const auto value = m_reply->rawHeader(name);
                if (!value.isEmpty() && !value.contains('\r') && !value.contains('\n')) wire += QByteArray(name) + ": " + value + "\r\n";
            }
            m_socket->write(wire + "Cache-Control: no-store\r\nConnection: close\r\n\r\n");
            m_sent = true;
            if (m_head) { close(); return; }
        }
        while (!m_closing && m_reply->isOpen() && m_reply->bytesAvailable() > 0 && m_socket->bytesToWrite() < outputBytes) {
            const auto bytes = m_reply->read(std::min({chunkBytes, m_reply->bytesAvailable(), outputBytes - m_socket->bytesToWrite()}));
            if (bytes.isEmpty()) break;
            if(m_source.resumeAttempts>0 && m_end>=0 && bytes.size()>m_end-m_start+1-m_bodyQueued){error(502);return;}
            if (m_socket->write(bytes) != bytes.size()) { close(); return; }
            m_bodyQueued+=bytes.size();
        }
        if (m_reply->isFinished() && m_reply->bytesAvailable() == 0) {
            if(m_end>=0 && m_bodyQueued<m_end-m_start+1 && resume())return;
            close();return;
        }
        if (m_socket->bytesToWrite() >= outputBytes || m_reply->bytesAvailable() > 0) m_timer.stop();
        else if (!m_timer.isActive()) m_timer.start();
    }
    QTcpSocket *m_socket;
    QNetworkAccessManager &m_manager;
    Source m_source;
    QByteArray m_route, m_request;
    QNetworkReply *m_reply = nullptr;
    QNetworkRequest m_upstream;
    qint64 m_start=0,m_end=-1,m_total=-1,m_bodyQueued=0;
    QByteArray m_validator,m_ifRange;
    int m_resumes=0; bool m_headersChecked=false;
    QTimer m_timer;
    bool m_started = false, m_sent = false, m_head = false, m_closing = false;
};

class Server final : public QObject {
public:
    explicit Server(Source source) : m_source(std::move(source))
    {
        m_route = '/' + randomBytes(32).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals) + "/media";
        connect(&m_listener, &QTcpServer::newConnection, this, [this] {
            while (m_listener.hasPendingConnections()) {
                auto *connection = new Connection(m_listener.nextPendingConnection(), m_manager, m_source, m_route, this);
                m_connections.insert(connection);
                connect(connection, &QObject::destroyed, this, [this, connection] { m_connections.remove(connection); });
            }
        });
        if (!m_listener.listen(QHostAddress::LocalHost, 0)) throw ProviderError("Cannot open the local media stream.");
    }
    ~Server() override { shutdown(); }
    QUrl url() const { return QUrl("http://127.0.0.1:" + QString::number(m_listener.serverPort()) + QString::fromLatin1(m_route)); }
    void shutdown()
    {
        m_listener.close();
        const auto connections = m_connections;
        for (auto *connection : connections) delete connection;
    }
private:
    Source m_source;
    QTcpServer m_listener;
    QNetworkAccessManager m_manager;
    QByteArray m_route;
    QSet<Connection *> m_connections;
};
}

struct RangeStream::State {
    std::unique_ptr<QThread> thread;
    Server *server = nullptr;
    QUrl url;
    std::mutex contextMutex, closeMutex;
    std::shared_ptr<std::atomic_int> resumed;
};

RangeStream::RangeStream(std::unique_ptr<State> state) : m_state(std::move(state)) {}
RangeStream::~RangeStream() { close(); }
QUrl RangeStream::url() const { return m_state->url; }
int RangeStream::resumedRequests() const {return m_state->resumed->load();}

std::unique_ptr<RangeStream> RangeStream::open(const ProviderConfig &config, const QString &episode, const Cancellation &cancel,int resumeAttempts)
{
    auto source = resolve(config, episode, cancel);
    source.resumeAttempts=std::clamp(resumeAttempts,0,8);source.resumed=std::make_shared<std::atomic_int>(0);
    checkCancelled(cancel);
    auto state = std::make_unique<State>(); state->resumed=source.resumed;
    auto *context = state.get();
    auto started = std::make_shared<std::promise<QUrl>>();
    auto future = started->get_future();
    state->thread.reset(QThread::create([context, source = std::move(source), started] {
        bool announced = false;
        try {
            QEventLoop loop;
            Server server(source);
            struct Publication {
                State *context;
                ~Publication() { std::lock_guard<std::mutex> guard(context->contextMutex); context->server = nullptr; }
            } publication{context};
            { std::lock_guard<std::mutex> guard(context->contextMutex); context->server = &server; }
            started->set_value(server.url());
            announced = true;
            loop.exec();
        } catch (...) { if (!announced) started->set_exception(std::current_exception()); }
    }));
    state->thread->start();
    try { state->url = future.get(); }
    catch (...) { state->thread->wait(); throw ProviderError("Cannot open the local media stream."); }
    auto result = std::unique_ptr<RangeStream>(new RangeStream(std::move(state)));
    checkCancelled(cancel);
    return result;
}

void RangeStream::close()
{
    std::lock_guard<std::mutex> closeGuard(m_state->closeMutex);
    if (!m_state->thread) return;
    {
        std::lock_guard<std::mutex> contextGuard(m_state->contextMutex);
        if (auto *server = m_state->server) QMetaObject::invokeMethod(server, [server] {
            server->shutdown(); QThread::currentThread()->quit();
        }, Qt::QueuedConnection);
    }
    m_state->thread->wait(); m_state->thread.reset();
}

} // namespace introvert::editor
