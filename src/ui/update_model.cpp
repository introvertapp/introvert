#include "update_model.h"
#include "providers/access_retry.h"
#include "detection/algorithm_package.h"
#include "detection/contracts.h"
#include <QJsonArray>
#include <QRunnable>
#include <QCoreApplication>
#include <QDateTime>
#include <QDirIterator>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QTimer>
#include <QSslError>
#include <algorithm>
#include <chrono>

namespace introvert::ui {
namespace {
QString runText(const QString &program, const QStringList &arguments)
{
    QProcess process; process.start(program, arguments);
    if (!process.waitForStarted(2000) || !process.waitForFinished(3000) || process.exitCode() != 0) {
        process.kill(); process.waitForFinished(); return {};
    }
    return QString::fromUtf8(process.readAllStandardOutput()).trimmed();
}
}
UpdateModel::UpdateModel(std::optional<SettingsStore> store, QObject *parent, QNetworkAccessManager *network, QUrl feed,
                         std::function<QString()> installationLocator, QString packageFormat)
    : QObject(parent), m_store(std::move(store)), m_network(this), m_transport(network ? network : &m_network), m_feed(std::move(feed)),
      m_installationLocator(std::move(installationLocator)), m_packageFormat(std::move(packageFormat))
{
    if (m_store) m_automatic = m_store->loadUpdates().value("automatic").toBool();
    m_timer.setInterval(60 * 60 * 1000);
    connect(&m_timer, &QTimer::timeout, this, &UpdateModel::automaticCheck);
    m_message = enabled() ? "Check for a new version of INTROvert." : "Update checks are disabled in UI preview.";
}
UpdateModel::~UpdateModel() { close(); }
QString UpdateModel::installedVersion() const { return QCoreApplication::applicationVersion().isEmpty() ? QStringLiteral(INTROVERT_VERSION) : QCoreApplication::applicationVersion(); }
void UpdateModel::start() { if (!m_closed && enabled()) {m_timer.start(); QTimer::singleShot(1000,this,&UpdateModel::automaticCheck);} }
void UpdateModel::automaticCheck()
{
    if (!m_store || m_closed || !m_automatic || m_busy || m_ready) return;
    try {
        const auto checked = m_store->loadUpdates().value("checked_at").toLongLong();
        const auto now = QDateTime::currentSecsSinceEpoch();
        if (checked <= 0 || checked > now || now - checked >= 24 * 60 * 60) check();
    } catch (...) { fail("Cannot read update preferences from app.db."); }
}
void UpdateModel::setAutomatic(bool automatic)
{
    if (!m_store || m_closed) return;
    try { m_store->setAutomaticUpdates(automatic); m_automatic = automatic; emit changed(); if(automatic)automaticCheck(); }
    catch (...) { fail("Cannot save update preferences in app.db."); }
}
void UpdateModel::check()
{
    if (!enabled() || m_closed || m_busy || m_ready) return;
    ++m_generation; m_offer = {}; m_appOffer = {}; m_detectorCandidate = {}; m_appCheckError.clear(); m_checkingDetector = false; m_detectorOffer = false; m_metadata.clear(); m_busy = true; m_progress = 0;
    m_message = "Checking for updates…"; emit changed();
    request(m_feed,false);
}
void UpdateModel::enableDetectorUpdates(QUrl feed)
{
    m_detectorFeed = std::move(feed);
    if (m_store) try { m_installedDetector = detection::AlgorithmPackages(m_store->databasePath()).selected().version; } catch (...) {}
}
void UpdateModel::checkDetector()
{
    m_appOffer = m_offer; m_offer = {}; m_checkingDetector = true; m_metadata.clear();
    try { m_installedDetector = detection::AlgorithmPackages(m_store->databasePath()).selected().version; }
    catch (...) { finishChecks("Cannot validate the installed detector package."); return; }
    request(m_detectorFeed, false);
}
void UpdateModel::finishChecks(const QString &error)
{
    m_detectorCandidate = error.isEmpty() ? m_offer : updates::Offer{};
    m_busy = false;
    if (!m_appOffer.version.isEmpty()) { m_offer = m_appOffer; m_detectorOffer = false; m_message = "INTROvert " + m_offer.version + " is available."; }
    else if (!m_offer.version.isEmpty()) { m_detectorOffer = true; m_message = "Detector " + m_offer.version + " is available (installed " + m_installedDetector + ")."; }
    else if (!error.isEmpty()) m_message = error;
    else if (!m_appCheckError.isEmpty()) m_message = m_appCheckError;
    else m_message = "INTROvert and detector are up to date. Detector " + m_installedDetector + ".";
    if (error.isEmpty() && m_appCheckError.isEmpty()) m_store->recordUpdateCheck(QDateTime::currentSecsSinceEpoch());
    emit changed();
}
void UpdateModel::checkFailed(const QString &message)
{
    if (m_detectorFeed.isEmpty()) { fail(message); return; }
    if (m_checkingDetector) { m_offer = {}; finishChecks(message); }
    else { m_appCheckError = message; m_offer = {}; checkDetector(); }
}
void UpdateModel::request(const QUrl &url, bool downloading, int redirects)
{
    if (m_closed) return;
    const auto origin=url.scheme()+"://"+url.host()+":"+QString::number(url.port());
    if(!m_access || m_accessOrigin!=origin){m_access=std::make_unique<providers::AccessRetry>(url);m_accessOrigin=origin;}
    QNetworkRequest outgoing(url);
    outgoing.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::ManualRedirectPolicy);
    outgoing.setRawHeader("User-Agent", "INTROvert/" + installedVersion().toUtf8());
    outgoing.setRawHeader("Accept", downloading ? "application/octet-stream" : "application/vnd.github+json");
    outgoing.setTransferTimeout(std::chrono::milliseconds(m_access->pending()?std::max(1,m_access->remainingMs()):60000));
    auto *reply = m_transport->get(outgoing); m_reply = reply; reply->setReadBufferSize(64*1024);
    auto certificateFailure=std::make_shared<bool>(false);
    connect(reply,&QNetworkReply::sslErrors,this,[certificateFailure](const QList<QSslError> &){*certificateFailure=true;});
    auto consume = [this,reply,downloading] {
        if (m_closed || m_reply != reply) return;
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status != 200) {reply->readAll(); return;}
        const auto data = reply->readAll();
        if (downloading) {
            m_received += data.size();
            if (m_received > m_offer.bytes || m_download.write(data) != data.size()) {fail("The update download is incomplete or exceeds its expected size.");return;}
            m_hash.addData(data); m_progress = int(100*m_received/m_offer.bytes); emit changed();
        } else {
            m_metadata += data;
            if (m_metadata.size() > 2*1024*1024) fail("The update response exceeds its supported size.");
        }
    };
    connect(reply,&QIODevice::readyRead,this,consume);
    connect(reply,&QNetworkReply::finished,this,[this,reply,downloading,redirects,consume,url,certificateFailure] {
        if (m_closed || m_reply != reply) {reply->deleteLater();return;}
        consume(); if (m_reply != reply) {reply->deleteLater();return;}
        m_reply.clear();
        const auto status=reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto next=reply->url().resolved(reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl());
        const auto networkError=reply->error(); reply->deleteLater();
        if(status==0 && !*certificateFailure && networkError!=QNetworkReply::OperationCanceledError
            && (downloading?m_received==0:m_metadata.isEmpty()) && m_access && m_access->pending()) {
            const auto generation=m_generation;
            m_message="Waiting for network access…";emit changed();
            QTimer::singleShot(std::min(500,m_access->remainingMs()),this,[this,url,downloading,redirects,generation] {
                if(m_closed || !m_busy || generation!=m_generation)return;
                if(!m_access || !m_access->pending()){fail("Cannot contact GitHub. Check network access and try again.");return;}
                request(url,downloading,redirects);
            });
            return;
        }
        if(m_access)m_access->complete();
        if (status>=300 && status<400 && downloading && redirects<5 && updates::trustedDownload(next)) {request(next,true,redirects+1);return;}
        if (status!=200 || networkError!=QNetworkReply::NoError) {
            if (downloading) fail(status==404 ? "Update information is unavailable. GitHub Releases must be publicly accessible."
                 : status==403 || status==429 ? "GitHub temporarily limited update checks. Try again later."
                 : "Cannot contact GitHub for updates. Check network access and try again.");
            else checkFailed(status == 404 ? "Update information is unavailable. GitHub Releases must be publicly accessible."
                : "Cannot contact GitHub for updates. Check network access and try again."); return;
        }
        if (downloading) {
            m_download.close();
            if (m_received!=m_offer.bytes || m_hash.result().toHex()!=m_offer.sha256) {fail("The update checksum did not match. The installed application was not changed.");return;}
            prepare(); return;
        }
        try {
            const auto document=QJsonDocument::fromJson(m_metadata);
            if (m_checkingDetector) {
                if (!document.isArray()) throw std::runtime_error("GitHub returned invalid detector release information.");
                m_offer = updates::detectorReleaseOffer(document.array(), m_installedDetector, installedVersion(), updates::platform(), updates::architecture(), QT_VERSION_STR);
                m_detectorLatestVersion = updates::latestDetectorVersion(document.array(), installedVersion(), updates::platform(), updates::architecture(), QT_VERSION_STR);
                finishChecks(); return;
            }
            if(!document.isObject())throw std::runtime_error("GitHub returned invalid release information.");
            m_offer=updates::releaseOffer(document.object(),installedVersion(),updates::platform(),updates::architecture(),m_packageFormat.isNull()?linuxFormat():m_packageFormat);
            m_latestVersion = updates::latestApplicationVersion(document.object());
            if (!m_detectorFeed.isEmpty()) { checkDetector(); return; }
            m_store->recordUpdateCheck(QDateTime::currentSecsSinceEpoch());
            m_busy=false;
            m_message=m_offer.version.isEmpty() ? "INTROvert is up to date." : "INTROvert "+m_offer.version+" is available.";
            emit changed();
        } catch(const std::exception &error) {checkFailed(QString::fromUtf8(error.what()));}
    });
}
QString UpdateModel::installationPath() const
{
#ifndef INTROVERT_PACKAGED
    return {};
#else
    const auto directory=QCoreApplication::applicationDirPath();
#ifdef Q_OS_MACOS
    const auto bundle=QDir::cleanPath(directory+"/../..");
    return QFileInfo(bundle).fileName()=="Introvert.app" && !QFileInfo(bundle).isSymLink() ? bundle : QString();
#elif defined(Q_OS_WIN)
    return QFileInfo(directory+"/Introvert.exe").isFile() ? directory : QString();
#else
    return directory=="/opt/introvert" && !linuxFormat().isEmpty() ? directory : QString();
#endif
#endif
}
QString UpdateModel::linuxFormat() const
{
#ifdef Q_OS_LINUX
    if(runText("/usr/bin/dpkg-query",{"-S","/opt/introvert/Introvert"}).startsWith("introvert:"))return "deb";
    if(runText("/usr/bin/rpm",{"-qf","--qf","%{NAME}","/opt/introvert/Introvert"})=="introvert")return "rpm";
#endif
    return {};
}
void UpdateModel::downloadDetector()
{
    if (m_closed || !enabled() || m_busy || m_ready || m_detectorCandidate.version.isEmpty()) return;
    m_detectorOffer = true; m_offer = m_detectorCandidate; download();
}
void UpdateModel::download()
{
    if(m_closed || !enabled() || m_busy || m_ready || m_offer.version.isEmpty())return;
    ++m_generation;
    m_target = m_detectorOffer ? QFileInfo(m_store->databasePath()).absolutePath() : m_installationLocator ? m_installationLocator() : installationPath();
    if(m_target.isEmpty()){fail("Updates can be installed only by a packaged INTROvert application.");return;}
    auto base=QDir::tempPath();
#ifdef Q_OS_MACOS
    base=m_detectorOffer ? m_target : QFileInfo(m_target).absolutePath();
    if(!QFileInfo(base).isWritable()){fail("Move INTROvert to a writable Applications location before updating.");return;}
#endif
    m_workspace=std::make_unique<QTemporaryDir>(base+"/.introvert-update-XXXXXX");
    if(!m_workspace->isValid()){fail("Cannot create a private update staging directory.");return;}
    m_payload=m_workspace->filePath(m_offer.name);
    m_download.setFileName(m_payload);
    if(!m_download.open(QIODevice::WriteOnly|QIODevice::NewOnly)){fail("Cannot save the application update.");return;}
    m_received=0; m_hash.reset(); m_busy=true; m_progress=0;
    m_message=QString("Downloading %1 %2…").arg(m_detectorOffer ? "detector" : "INTROvert", m_offer.version); emit changed(); request(m_offer.download,true);
}
void UpdateModel::prepare()
{
    if (m_detectorOffer) {
        m_message = "Verifying detector update…"; emit changed();
        const auto cancelled = m_installCancel = std::make_shared<std::atomic_bool>(false);
        const auto outcome = m_installResult = std::make_shared<QString>();
        const auto database = m_store->databasePath(), payload = m_payload, version = m_offer.version; const auto generation = m_generation;
        m_installPool.start(QRunnable::create([this, database, payload, version, cancelled, generation, outcome] {
            QString installed;
            try { installed = detection::AlgorithmPackages(database).install(payload, version, [cancelled] { if (cancelled->load()) throw detection::Cancelled(); }).version; } catch (...) {}
            *outcome = installed;
            QMetaObject::invokeMethod(this, [this, installed, generation] {
                if (m_closed || generation != m_generation) return;
                if (installed.isEmpty()) { fail("Detector update verification failed. The previous detector remains available."); return; }
                m_installedDetector = installed; m_detectorCandidate = {}; m_offer = m_appOffer; m_detectorOffer = false;
                m_busy = false; m_ready = false; m_progress = 0; m_workspace.reset();
                m_installResult.reset(); m_installCancel.reset();
                m_message = "Detector " + installed + " installed. New scans use this version; running scans keep their current version.";
                if (!m_appOffer.version.isEmpty()) m_message += " INTROvert " + m_appOffer.version + " is also available.";
                emit changed();
            }, Qt::QueuedConnection);
        }));
        return;
    }
#ifdef Q_OS_MACOS
    QDir().mkpath(m_workspace->filePath("expanded"));
    prepareStep(0);
#else
    m_ready=true; m_busy=false; m_message="Update verified. Restart to install INTROvert "+m_offer.version+"."; emit changed();
#endif
}
void UpdateModel::prepareStep(int step)
{
    m_message="Verifying application update…"; emit changed();
    disconnect(&m_process,nullptr,this,nullptr);
    connect(&m_process,&QProcess::errorOccurred,this,[this]{fail("Cannot prepare the application update.");});
    connect(&m_process,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,[this,step](int code,QProcess::ExitStatus status){
        if(m_closed)return;
        if(code!=0 || status!=QProcess::NormalExit){fail("The staged application could not be verified. INTROvert was not changed.");return;}
        const auto output=m_process.readAllStandardOutput();
        if(step==0) {
            for(const auto &entry:output.split('\n')) {
                if(entry.isEmpty())continue;
                if(entry.startsWith('/') || entry.contains("..") || entry.contains('\\')
                    || !(entry=="Introvert.app" || entry.startsWith("Introvert.app/") || entry.startsWith("__MACOSX/"))) {
                    fail("The update archive contains an unsupported path.");return;
                }
            }
        }
        if(step==2) {
            const auto metadata=QJsonDocument::fromJson(output).object();
            if(metadata["CFBundleIdentifier"]!="org.introvertapp.Introvert" || metadata["CFBundleExecutable"]!="Introvert"
                || metadata["CFBundleShortVersionString"].toString()!=m_offer.version) {
                fail("The staged application identity does not match this update.");return;
            }
        }
        if(step<3){prepareStep(step+1);return;}
        const auto root=QFileInfo(m_workspace->filePath("expanded")).canonicalFilePath()+"/";
        QDirIterator items(root,QDir::AllEntries|QDir::NoDotAndDotDot|QDir::Hidden,QDirIterator::Subdirectories);
        while(items.hasNext()) {items.next(); if(items.fileInfo().isSymLink() && !items.fileInfo().canonicalFilePath().startsWith(root)) {
            fail("The staged application contains an unsafe link.");return;
        }}
        m_payload=m_workspace->filePath("expanded/Introvert.app");
        m_busy=false; m_ready=true; m_message="Update verified. Restart to install INTROvert "+m_offer.version+"."; emit changed();
    });
    if(step==0)m_process.start("/usr/bin/tar",{"-tf",m_payload});
    else if(step==1)m_process.start("/usr/bin/tar",{"-xf",m_payload,"-C",m_workspace->filePath("expanded")});
    else if(step==2)m_process.start("/usr/bin/plutil",{"-convert","json","-o","-",m_workspace->filePath("expanded/Introvert.app/Contents/Info.plist")});
    else m_process.start("/usr/bin/codesign",{"--verify","--deep","--strict",m_workspace->filePath("expanded/Introvert.app")});
}
void UpdateModel::install()
{
    if(m_closed || !m_ready || m_busy)return;
    if(m_restartGuard && !m_restartGuard()){m_message="Close the video editor and wait for active work to finish before updating.";emit changed();return;}
    const QString suffix=updates::platform()=="windows" ? ".exe" : "";
    const auto source=QCoreApplication::applicationDirPath()+"/introvert-update"+suffix;
    const auto helper=m_workspace->filePath("introvert-update"+suffix);
    if(!QFile::copy(source,helper) || !QFile::setPermissions(helper,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner)) {
        fail("The application update helper is missing or cannot be staged.");return;
    }
    QByteArray random(32,0); for(auto &byte:random)byte=char(QRandomGenerator::system()->generate() & 255);
    const auto token=QString::fromLatin1(random.toHex());
    QStringList arguments{QString::number(QCoreApplication::applicationPid()),m_target,m_payload,m_workspace->path(),token,m_offer.version};
    arguments+=m_restartArguments;
    QProcess launcher;launcher.setProgram(helper);launcher.setArguments(arguments);launcher.setWorkingDirectory(m_workspace->path());
#ifdef Q_OS_WIN
    launcher.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments){arguments->flags|=0x08000000;}); // CREATE_NO_WINDOW
#endif
    if(!launcher.startDetached()){QFile::remove(helper);fail("Cannot start the application update helper.");return;}
    m_workspace->setAutoRemove(false); m_handedOff=true; m_busy=true; m_message="Restarting to install the update…"; emit changed(); emit restartRequested();
}
void UpdateModel::reportFailedInstallation()
{
    m_message="The application update failed. Try again, or reinstall INTROvert from GitHub Releases.";emit changed();
}
void UpdateModel::cancel()
{
    if(m_closed || m_handedOff)return;
    ++m_generation;m_access.reset();m_accessOrigin.clear();
    if(m_reply){auto reply=m_reply;m_reply.clear();reply->abort();reply->deleteLater();}
    disconnect(&m_process,nullptr,this,nullptr);
    if(m_process.state()!=QProcess::NotRunning){m_process.kill();m_process.waitForFinished(3000);}
    if (m_installCancel) m_installCancel->store(true); m_installPool.waitForDone(); m_installCancel.reset();
    m_download.close();m_workspace.reset();m_busy=false;m_ready=false;m_progress=0;
    if (m_installResult && !m_installResult->isEmpty()) {
        m_installedDetector = *m_installResult; m_detectorCandidate = {}; m_offer = m_appOffer; m_detectorOffer = false;
        m_message = "Detector " + *m_installResult + " installed. New scans use this version.";
    }
    else m_message=m_detectorOffer ? "Detector update cancelled. The previous detector remains available." : "Update cancelled. The installed application was not changed.";
    if (m_detectorOffer && !m_appOffer.version.isEmpty()) { m_offer = m_appOffer; m_detectorOffer = false; }
    m_installResult.reset(); emit changed();
}
void UpdateModel::fail(const QString &message)
{
    if(m_closed)return;
    cancel();m_message=message;emit changed();
}
void UpdateModel::close()
{
    if(m_closed)return;
    // A launched helper owns the retained staging directory after application exit.
    cancel();m_closed=true;m_timer.stop();
}
}
