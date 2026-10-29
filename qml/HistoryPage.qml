import QtQuick
import QtQuick.Layouts
Item {
    id: page
    property var theme
    property bool preview: false
    property var items: []
    property var statistics: ({})
    property string query: ""
    property string visualState: "ready"
    signal showOpened(var record)
    objectName: "historyPage"
    ColumnLayout {
        anchors.fill: parent; spacing: 12
        StatCards {
            Layout.fillWidth: true; theme: page.theme
            entries: ["Shows","Episodes","Recap","Intro","Credits","Preview","IntroDB","TheIntroDB"].map(function(label,i) { return {label:label,value:page.preview ? [3,3,1,1,1,0,2,1][i] : page.statistics[label] === undefined ? 0 : page.statistics[label]} })
        }
        JobsTable { unavailableMessage: ""; Layout.fillWidth: true; Layout.fillHeight: true; Layout.maximumHeight: implicitHeight; theme: page.theme; history: true; items: page.items; query: page.query; visualState: page.visualState; onShowOpened: function(record) { page.showOpened(record) } }
        Item { Layout.fillHeight: true }
    }
}
