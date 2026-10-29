#pragma once
#include <QObject>
#include <QPointer>
#include <QWindow>

namespace introvert {
class WindowLifecycle final : public QObject {
    Q_OBJECT
public:
    explicit WindowLifecycle(QWindow *window, QObject *parent = nullptr);
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    QPointer<QWindow> m_window;
};
}
