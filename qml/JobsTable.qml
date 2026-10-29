import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: table
    required property var theme
    property var items: []
    property bool history: false
    property string query: ""
    property string unavailableMessage: ""
    property string visualState: "ready"
    property string sortKey: "date"
    property bool descending: history
    property int pageNumber: 0
    readonly property var columns: history ? [{key:"type",label:"Type",weight:120},{key:"endpoint",label:"Endpoint",weight:150},{key:"show",label:"Show Title",weight:220},{key:"episode",label:"Episode Title",weight:260},{key:"code",label:"Season/Episode",weight:150},{key:"date",label:"Date/Time Added",weight:210},{key:"status",label:"Status",weight:150}] : [{key:"type",label:"Type",weight:120},{key:"show",label:"Show Title",weight:220},{key:"episode",label:"Episode Title",weight:260},{key:"code",label:"Season/Episode",weight:150},{key:"date",label:"Date/Time Added",weight:210},{key:"status",label:"Status",weight:150}]
    readonly property real totalWeight: columns.reduce(function(total,c) { return total+c.weight },0)
    readonly property var sortedRows: items.filter(function(row) { return JSON.stringify(row).toLowerCase().indexOf(query.toLowerCase()) >= 0 }).slice().sort(function(a,b) { return String(a[sortKey]).localeCompare(String(b[sortKey])) * (descending ? -1 : 1) })
    readonly property int displayedCount: visualState === "ready" ? Math.min(100, Math.max(0, sortedRows.length-pageNumber*100)) : 0
    onItemsChanged: pageNumber=Math.min(pageNumber,Math.max(0,Math.ceil((sortedRows ? sortedRows.length : 0)/100)-1))
    onQueryChanged: pageNumber=0
    implicitHeight: 40 + Math.max(66, displayedCount*41) + (displayedCount > 0 ? 58 : 0)
    property var columnOverrides: ({})
    function columnWidth(column) { return columnOverrides[column.key] || (history ? viewport.width : Math.max(viewport.width,1110)) * column.weight / totalWeight }
    function displayValue(record, key) {
        if(key !== "date") return String(record[key])
        var date = new Date(record[key])
        return isNaN(date.getTime()) ? String(record[key]) : date.toLocaleString(Qt.locale(), Locale.ShortFormat)
    }
    signal showOpened(var record)
    objectName: history ? "historyTable" : "queueTable"
    color: theme.card; border.color: theme.border; radius: 12; clip: true
    Flickable {
        id: viewport
        anchors.fill: parent; anchors.margins: 1
        contentWidth: Math.max(width,table.columns.reduce(function(total,column){return total+table.columnWidth(column)},0)); contentHeight: height
        clip: true; boundsBehavior: Flickable.StopAtBounds
        ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AlwaysOff }
        ColumnLayout {
            width: viewport.contentWidth; height: viewport.height; spacing: 0
            Row {
                Layout.fillWidth: true; height: 40
                Repeater {
                    model: table.columns
                    AbstractButton {
                        HoverHandler { cursorShape: Qt.PointingHandCursor }
                        required property var modelData
                        width: table.columnWidth(modelData); height: 40
                        objectName: "sort_" + modelData.key
                        hoverEnabled: true
                        onClicked: { table.descending = table.sortKey === modelData.key ? !table.descending : false; table.sortKey = modelData.key }
                        background: Rectangle { color: parent.hovered ? table.theme.hover : table.theme.field }
                        Rectangle { anchors.right: parent.right; height: parent.height; width: 1; color: table.theme.border }
                        MouseArea {
                            anchors.right: parent.right; width: 7; height: parent.height; cursorShape: Qt.SplitHCursor
                            property real startX: 0
                            property real startWidth: 0
                            onPressed: function(mouse) { startX=mapToItem(viewport,mouse.x,0).x; startWidth=parent.width }
                            onPositionChanged: function(mouse) {
                                if(pressed) { var values=Object.assign({},table.columnOverrides); values[parent.modelData.key]=Math.max(80,startWidth+mapToItem(viewport,mouse.x,0).x-startX); table.columnOverrides=values }
                            }
                        }
                        contentItem: Text { leftPadding: 14; verticalAlignment: Text.AlignVCenter; text: modelData.label + (table.sortKey === modelData.key ? table.descending ? " ˅" : " ˄" : " -"); color: table.theme.muted; font.pixelSize: 13; font.bold: true; elide: Text.ElideRight }
                    }
                }
            }
            ListView {
                id: rows
                objectName: "jobRows"
                Layout.fillWidth: true; Layout.fillHeight: true; clip: true
                model: table.visualState === "ready" ? table.sortedRows.slice(table.pageNumber*100, (table.pageNumber+1)*100) : []
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AlwaysOff }
                delegate: Rectangle {
                    id: row
                    required property var modelData
                    required property int index
                    width: rows.width; height: 41; color: "transparent"
                    property var rowRecord: modelData
                    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: table.theme.border }
                    Row {
                        Repeater {
                            model: table.columns
                            Text {
                                required property var modelData
                                width: table.columnWidth(modelData); height: 41
                                leftPadding: 14; rightPadding: 10; verticalAlignment: Text.AlignVCenter
                                text: table.displayValue(row.rowRecord, modelData.key); elide: Text.ElideRight; font.pixelSize: 13
                                color: modelData.key === "show" ? table.theme.link : modelData.key === "status" ? (row.rowRecord.status === "Failed" ? table.theme.danger : row.rowRecord.status.indexOf("Rate") >= 0 ? table.theme.warning : table.theme.text) : table.theme.text
                                Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: table.theme.border }
                                MouseArea { anchors.fill: parent; enabled: modelData.key === "show"; cursorShape: Qt.PointingHandCursor; onClicked: table.showOpened(row.rowRecord) }
                            }
                        }
                    }
                }
                Text {
                    anchors.centerIn: parent
                    AppToolTip { visible: emptyHover.hovered && table.unavailableMessage !== ""; text: table.unavailableMessage; theme: table.theme }
                    HoverHandler { id: emptyHover }
                    text: table.visualState === "loading" ? "Loading…" : table.visualState === "error" ? "Unable to load items. Retry when the service is connected." : table.query ? "No items match your search." : table.history ? "History is empty." : "Queue is empty."
                    visible: rows.count === 0; color: table.visualState === "error" ? table.theme.danger : table.theme.muted; font.pixelSize: 14
                }
            }
            RowLayout {
                visible: table.displayedCount > 0
                Layout.fillWidth: true; Layout.margins: visible ? 12 : 0
                Text { text: (table.pageNumber*100+1) + "-" + Math.min((table.pageNumber+1)*100,table.sortedRows.length) + " of " + table.sortedRows.length; color: table.theme.muted; font.pixelSize: 13 }
                Item { Layout.fillWidth: true }
                AppButton { text: "Previous"; enabled: table.pageNumber > 0; theme: table.theme; onClicked: table.pageNumber-- }
                Text { text: "Page " + (table.pageNumber+1) + " of " + Math.max(1,Math.ceil(table.sortedRows.length/100)); color: table.theme.muted }
                AppButton { text: "Next"; enabled: (table.pageNumber+1)*100 < table.sortedRows.length; theme: table.theme; onClicked: table.pageNumber++ }
            }
        }
    }
    RoundedFrame { anchors.fill: parent; radius: table.radius; outsideColor: table.theme.background; borderColor: table.theme.border }

}
