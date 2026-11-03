import QtQuick
import QtQuick.Layouts
Item {
    id: page
    property var theme
    property bool preview: false
    property var items: []
    readonly property var requestedOrder: jobs.requestedOrder
    property var queuePositions: ({})
    signal moveRequested(string key, string target, bool after)
    property string query: ""
    property string visualState: "ready"
    signal showOpened(var record)
    function refreshOrder() { if (!jobs.dragging) { jobs.optimisticOrder = []; jobs.synchronize() } }
    objectName: "queuePage"
    ColumnLayout {
        anchors.fill: parent; spacing: 12
        JobsTable { id: jobs; unavailableMessage: ""; Layout.fillWidth: true; Layout.fillHeight: true; Layout.maximumHeight: implicitHeight; theme: page.theme; items: page.items; queuePositions: page.queuePositions; onMoveRequested: function(key, target, after) { page.moveRequested(key, target, after) }; query: page.query; visualState: page.visualState; onShowOpened: function(record) { page.showOpened(record) } }
        Item { Layout.fillHeight: true }
    }
}
