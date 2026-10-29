#include "access_retry.h"
#include <QFile>
#include <QHostAddress>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QThread>
#include <cerrno>
#include <algorithm>
#include <QDir>
#include <QFileInfo>
#ifdef Q_OS_MACOS
#include <dirent.h>
#endif

namespace introvert::providers {
namespace { QMutex mutex; QSet<QString> completed; }
bool AccessRetry::defaultEnabled()
{
#ifdef Q_OS_MACOS
    return true;
#else
    return false;
#endif
}
AccessRetry::AccessRetry(const QUrl &url, bool enabled, int budgetMs)
    : m_origin(url.scheme()+"://"+url.host()+":"+QString::number(url.port())), m_enabled(enabled), m_budgetMs(budgetMs)
{
    // Local bridge/test sockets do not need a remote-network permission grace.
    if(url.host()=="localhost" || QHostAddress(url.host()).isLoopback())m_enabled=false;
    QMutexLocker lock(&mutex); if(completed.contains(m_origin))m_enabled=false;
    m_clock.start();
}
int AccessRetry::remainingMs() const {return int(std::max<qint64>(0,m_budgetMs-m_clock.elapsed()));}
bool AccessRetry::pending() const {return m_enabled && remainingMs()>0;}
void AccessRetry::complete() {if(m_enabled){QMutexLocker lock(&mutex);completed.insert(m_origin);}m_enabled=false;}
void AccessRetry::wait(const Cancellation &cancelled) const
{
    const auto until=m_clock.elapsed()+std::min(remainingMs(),500);
    while(m_clock.elapsed()<until){checkCancelled(cancelled);QThread::msleep(25);}
    checkCancelled(cancelled);
}
bool readableMediaFile(const QString &path,const Cancellation &cancelled)
{
    QElapsedTimer clock;clock.start();
    for(;;) {
        checkCancelled(cancelled);
        const QFileInfo info(path);
        if(info.exists() && !info.isFile())return false;
        QFile file(path);errno=0;
        if(file.open(QIODevice::ReadOnly)){file.read(1);return file.error()==QFileDevice::NoError;}
        const auto reason=errno;
        const bool denied=reason==EACCES || reason==EPERM || file.error()==QFileDevice::PermissionsError;
        if(!AccessRetry::defaultEnabled() || !denied || clock.elapsed()>=60000)return false;
        for(int i=0;i<20;++i){checkCancelled(cancelled);QThread::msleep(25);}
    }
}
bool readableMediaDirectory(const QString &path,const Cancellation &cancelled)
{
#ifdef Q_OS_MACOS
    QElapsedTimer clock;clock.start();
    for(;;) {
        checkCancelled(cancelled);
        if(auto *directory=opendir(QFile::encodeName(path).constData())) {closedir(directory);return true;}
        if((errno!=EACCES && errno!=EPERM) || clock.elapsed()>=60000)return false;
        for(int i=0;i<20;++i){checkCancelled(cancelled);QThread::msleep(25);}
    }
#else
    checkCancelled(cancelled);return QFileInfo(path).isDir() && QDir(path).isReadable();
#endif
}
}
