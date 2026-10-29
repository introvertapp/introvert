#pragma once

#include <QObject>
#include <QString>

namespace introvert {

class NavigationModel final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString page READ page NOTIFY pageChanged)

public:
    explicit NavigationModel(QObject *parent = nullptr);
    QString page() const;
    Q_INVOKABLE void navigate(const QString &page);

signals:
    void pageChanged();

private:
    QString m_page = QStringLiteral("home");
};

} // namespace introvert
