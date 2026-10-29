#include "navigation_model.h"

#include <QStringList>

namespace introvert {

NavigationModel::NavigationModel(QObject *parent) : QObject(parent) {}

QString NavigationModel::page() const { return m_page; }

void NavigationModel::navigate(const QString &page)
{
    static const QStringList pages = {"home", "queue", "history", "settings"};
    if (pages.contains(page) && page != m_page) {
        m_page = page;
        emit pageChanged();
    }
}

} // namespace introvert
