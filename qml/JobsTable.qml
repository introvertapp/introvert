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
    property string sortKey: history ? "date" : ""
    property var queuePositions: ({})
    property var optimisticOrder: []
    property var requestedOrder: []
    readonly property var optimisticRanks: {
        var ranks = {}
        for (var i = 0; i < optimisticOrder.length; ++i) ranks[optimisticOrder[i]] = i
        return ranks
    }
    property string draggedKey: ""
    property real dragY: 0
    property real pointerY: 0
    property real pointerOffset: 0
    property string runningKeys: ""
    readonly property bool dragging: draggedKey !== ""
    readonly property var orderedRows: history ? items.slice().sort(function(a,b) { return String(a[sortKey]).localeCompare(String(b[sortKey])) * (descending ? -1 : 1) }) : items.slice().sort(queueCompare)
    property bool descending: history
    property int pageNumber: 0
    readonly property var columns: history ? [{key:"type",label:"Type",weight:120},{key:"endpoint",label:"Endpoint",weight:150},{key:"show",label:"Show Title",weight:220},{key:"episode",label:"Episode Title",weight:260},{key:"code",label:"Season/Episode",weight:150},{key:"date",label:"Date/Time Added",weight:210},{key:"status",label:"Status",weight:150}] : [{key:"type",label:"Type",weight:120},{key:"show",label:"Show Title",weight:220},{key:"episode",label:"Episode Title",weight:260},{key:"code",label:"Season/Episode",weight:150},{key:"date",label:"Date/Time Added",weight:210},{key:"status",label:"Status",weight:150}]
    readonly property real totalWeight: columns.reduce(function(total,c) { return total+c.weight },0)
    readonly property var sortedRows: query === "" ? orderedRows : orderedRows.filter(function(row) { return JSON.stringify(row).toLowerCase().indexOf(query.toLowerCase()) >= 0 })
    readonly property int displayedCount: visualState === "ready" ? (history ? Math.min(100, Math.max(0, sortedRows.length-pageNumber*100)) : sortedRows.length) : 0
    onItemsChanged: pageNumber=Math.min(pageNumber,Math.max(0,Math.ceil((sortedRows ? sortedRows.length : 0)/100)-1))
    onQueryChanged: pageNumber=0
    implicitHeight: 40 + Math.max(66, displayedCount*41) + (history && displayedCount > 0 ? 58 : 0)
    property var columnOverrides: ({})
    function columnWidth(column) { return columnOverrides[column.key] || (history ? viewport.width : Math.max(viewport.width,1110)) * column.weight / totalWeight }
    function displayValue(record, key) {
        if(key !== "date") return String(record[key])
        var date = new Date(record[key])
        return isNaN(date.getTime()) ? String(record[key]) : date.toLocaleString(Qt.locale(), Locale.ShortFormat)
    }
    function rowKey(record) {
        return record.queueKey || (record.type + ":" + record.jobId + ":" + record.code)
    }
    function isRunning(record) {
        return record.queueRunning === true || record.status === "Submitting" || record.status === "Detecting" || String(record.status).indexOf("Running") === 0
    }
    function canMove(record) {
        return !history && !isRunning(record) && (!!record.queueKey || record.status === "Queued" || String(record.status).indexOf("Rate limited") === 0)
    }
    function queueCompare(a, b) {
        if (isRunning(a) !== isRunning(b)) return isRunning(a) ? -1 : 1
        if (sortKey !== "") return String(a[sortKey]).localeCompare(String(b[sortKey])) * (descending ? -1 : 1)
        var ai = optimisticRanks[rowKey(a)], bi = optimisticRanks[rowKey(b)]
        if (ai !== undefined && bi !== undefined) return ai - bi
        var ap = queuePositions[rowKey(a)] !== undefined ? queuePositions[rowKey(a)] : a.queueOrder
        var bp = queuePositions[rowKey(b)] !== undefined ? queuePositions[rowKey(b)] : b.queueOrder
        if (ap !== undefined && ap !== null && bp !== undefined && bp !== null) return ap - bp
        return String(a.date).localeCompare(String(b.date))
    }
    function synchronize() {
        var desired = visualState === "ready" ? (history ? sortedRows.slice(pageNumber*100, (pageNumber+1)*100) : sortedRows) : []
        if (history) {
            // History has per-segment records, including distinct provider items
            // with identical display fields. Preserve every row as before.
            rowModel.clear()
            for (var h = 0; h < desired.length; ++h) rowModel.append({record: desired[h]})
            return
        }
        var active = desired.filter(isRunning).map(rowKey).join("|")
        if (dragging && (active !== runningKeys || !desired.some(function(r) { return rowKey(r) === draggedKey }))) endDrag(false)
        runningKeys = active
        // Keep delegates alive during progress refreshes. Replacing a JS-array
        // model would destroy the grabbed row and restart every animation.
        var wanted = {}, indexes = {}
        for (var d = 0; d < desired.length; ++d) wanted[rowKey(desired[d])] = true
        for (var i = rowModel.count - 1; i >= 0; --i)
            if (!wanted[rowKey(rowModel.get(i).record)]) rowModel.remove(i)
        if (dragging) for (var n = 0; n < rowModel.count; ++n) indexes[rowKey(rowModel.get(n).record)] = n
        for (var j = 0; j < desired.length; ++j) {
            var key = rowKey(desired[j])
            var found = dragging ? indexes[key] : j < rowModel.count && rowKey(rowModel.get(j).record) === key ? j : undefined
            if (found === undefined && !dragging)
                for (var k = j+1; k < rowModel.count; ++k) if (rowKey(rowModel.get(k).record) === key) { found = k; break }
            if (found === undefined) {
                var insertAt = dragging ? rowModel.count : j
                rowModel.insert(insertAt, {record: desired[j]})
                indexes[key] = insertAt
            } else {
                if (!dragging && found !== j) { rowModel.move(found, j, 1); found = j }
                if (JSON.stringify(rowModel.get(found).record) !== JSON.stringify(desired[j])) rowModel.setProperty(found, "record", desired[j])
            }
        }
    }
    function beginDrag(record, scenePoint, rowTop) {
        if (!canMove(record)) return
        draggedKey = rowKey(record)
        pointerY = rows.mapFromItem(null, scenePoint.x, scenePoint.y).y
        pointerOffset = pointerY + rows.contentY - rowTop
        dragY = rowTop
    }
    function updateDrag(scenePoint) {
        if (!dragging) return
        pointerY = rows.mapFromItem(null, scenePoint.x, scenePoint.y).y
        moveAtPointer()
    }
    function moveAtPointer() {
        if (!dragging) return
        var first = 0, from = -1
        for (var i = 0; i < rowModel.count; ++i) {
            if (isRunning(rowModel.get(i).record)) first = i + 1
            if (rowKey(rowModel.get(i).record) === draggedKey) from = i
        }
        if (from < first) { endDrag(false); return }
        dragY = Math.max(first*41, Math.min((rowModel.count-1)*41, pointerY + rows.contentY - pointerOffset))
        var to = Math.max(first, Math.min(rowModel.count-1, Math.floor((dragY + 20.5)/41)))
        if (from !== to) rowModel.move(from, to, 1)
    }
    function endDrag(save) {
        if (!dragging) return
        var key = draggedKey, from = -1
        for (var i = 0; i < rowModel.count; ++i) if (rowKey(rowModel.get(i).record) === key) from = i
        var target = from > 0 && !isRunning(rowModel.get(from-1).record) ? rowKey(rowModel.get(from-1).record)
                   : from >= 0 && from+1 < rowModel.count ? rowKey(rowModel.get(from+1).record) : ""
        var after = from > 0 && !isRunning(rowModel.get(from-1).record)
        if (save && target !== "") {
            var keys = orderedRows.map(rowKey)
            keys.splice(keys.indexOf(key), 1)
            keys.splice(keys.indexOf(target) + (after ? 1 : 0), 0, key)
            optimisticOrder = keys
            requestedOrder = keys
            sortKey = ""
        }
        draggedKey = ""
        if (save && target !== "") moveRequested(key, target, after)
        synchronize()
    }
    onSortedRowsChanged: synchronize()
    onPageNumberChanged: synchronize()
    onVisualStateChanged: synchronize()
    onQueuePositionsChanged: { if (!dragging) optimisticOrder = []; synchronize() }
    onVisibleChanged: if (!visible) endDrag(false)
    Component.onCompleted: synchronize()
    ListModel { id: rowModel; dynamicRoles: true }
    Timer {
        interval: 16; repeat: true; running: table.dragging
        onTriggered: {
            var distance = table.pointerY < 36 ? table.pointerY - 36 : table.pointerY > rows.height-36 ? table.pointerY - (rows.height-36) : 0
            if (distance !== 0) {
                rows.contentY = Math.max(0, Math.min(Math.max(0, rows.contentHeight-rows.height), rows.contentY + Math.max(-14, Math.min(14, distance*.3))))
                table.moveAtPointer()
            }
        }
    }
    signal moveRequested(string key, string target, bool after)
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
                        contentItem: Text { leftPadding: 14; verticalAlignment: Text.AlignVCenter; text: modelData.label + (table.sortKey === modelData.key ? table.descending ? " ˅" : " ˄" : table.history ? " -" : ""); color: table.theme.muted; font.pixelSize: 13; font.bold: true; elide: Text.ElideRight }
                    }
                }
            }
            ListView {
                id: rows
                objectName: "jobRows"
                Layout.fillWidth: true; Layout.fillHeight: true; clip: true
                model: rowModel
                interactive: !table.dragging
                boundsBehavior: Flickable.StopAtBounds
                move: Transition { NumberAnimation { properties: "x,y"; duration: 180; easing.type: Easing.OutCubic } }
                moveDisplaced: Transition { NumberAnimation { properties: "x,y"; duration: 180; easing.type: Easing.OutCubic } }
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AlwaysOff }
                delegate: Item {
                    id: row
                    required property var record
                    required property int index
                    objectName: "jobRow_" + table.rowKey(record)
                    width: rows.width; height: 41
                    property var rowRecord: record
                    z: table.draggedKey === table.rowKey(record) ? 10 : 0
                    Rectangle {
                        id: rowSurface
                        width: row.width; height: row.height
                        y: table.draggedKey === table.rowKey(row.record) ? table.dragY-row.y : 0
                        color: table.draggedKey === table.rowKey(row.record) ? table.theme.hover : table.theme.card
                        border.width: table.draggedKey === table.rowKey(row.record) ? 1 : 0
                        border.color: table.theme.border
                        Behavior on y { enabled: table.draggedKey !== table.rowKey(row.record); NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }
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
                                    DragHandler {
                                        id: rowDrag
                                        target: null
                                        enabled: modelData.key !== "show" && table.canMove(row.rowRecord)
                                        xAxis.enabled: false
                                        cursorShape: active ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                                        onActiveChanged: {
                                            if (active) table.beginDrag(row.rowRecord, centroid.scenePosition, row.y)
                                            else table.endDrag(true)
                                        }
                                        onCentroidChanged: if (active) table.updateDrag(centroid.scenePosition)
                                    }
                                    MouseArea { anchors.fill: parent; enabled: modelData.key === "show"; cursorShape: Qt.PointingHandCursor; onClicked: table.showOpened(row.rowRecord) }
                                }
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
                visible: table.history && table.displayedCount > 0
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
