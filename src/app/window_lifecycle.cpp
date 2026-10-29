#include "window_lifecycle.h"
#include <QCoreApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QTimer>

namespace introvert {
WindowLifecycle::WindowLifecycle(QWindow *window, QObject *parent) : QObject(parent), m_window(window)
{
#ifdef Q_OS_MACOS
    QCoreApplication::instance()->installEventFilter(this);
#endif
}
bool WindowLifecycle::eventFilter(QObject *, QEvent *event)
{
#ifdef Q_OS_MACOS
    if ((event->type()==QEvent::ApplicationActivate || (event->type()==QEvent::ApplicationStateChange
         && QGuiApplication::applicationState()==Qt::ApplicationActive)) && m_window && !m_window->isVisible()) {
        QTimer::singleShot(0,this,[this]{if(m_window){m_window->show();m_window->raise();m_window->requestActivate();}});
    }
#else
    Q_UNUSED(event);
#endif
    return false;
}
}
