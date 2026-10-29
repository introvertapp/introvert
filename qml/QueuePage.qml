import QtQuick
import QtQuick.Layouts
Item {
    id: page
    property var theme
    property bool preview: false
    property var items: []
    property string query: ""
    property string visualState: "ready"
    signal showOpened(var record)
    objectName: "queuePage"
    ColumnLayout {
        anchors.fill: parent; spacing: 12
        JobsTable { unavailableMessage: ""; Layout.fillWidth: true; Layout.fillHeight: true; Layout.maximumHeight: implicitHeight; theme: page.theme; items: page.items; query: page.query; visualState: page.visualState; onShowOpened: function(record) { page.showOpened(record) } }
        Item { Layout.fillHeight: true }
    }
}
