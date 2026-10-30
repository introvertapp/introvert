import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: page
    property var theme
    property var backend: null
    property bool autoScroll: true
    spacing: 12
    function scrollToTail() { if(autoScroll) logList.positionViewAtEnd() }
    function followTail() { if(autoScroll) Qt.callLater(scrollToTail) }
    Text {
        Layout.fillWidth: true
        text: "Backend events and errors from this session. Up to 1,000 entries are kept in memory."
        textFormat: Text.PlainText; wrapMode: Text.Wrap
        color: page.theme.muted; font.pixelSize: 13
    }
    RowLayout {
        Layout.fillWidth: true; spacing: 8
        AppTextField {
            objectName: "logSearchControl"
            Layout.fillWidth: true; Layout.minimumWidth: 100
            placeholderText: "Search logs…"
            text: page.backend ? page.backend.search : ""
            onTextEdited: if(page.backend) page.backend.search = text
            Accessible.name: "Search logs"
        }
        AppComboBox {
            objectName: "logLevelControl"
            Layout.preferredWidth: 128
            model: ["All levels", "Info", "Warnings", "Errors", "Debug"]
            currentIndex: page.backend ? ["all","info","warning","error","debug"].indexOf(page.backend.level) : 0
            onActivated: function(index) { if(page.backend) page.backend.level = ["all","info","warning","error","debug"][index] }
            Accessible.name: "Log level"
        }
        AppButton {
            objectName: "clearLogsControl"; theme: page.theme; text: "Clear"
            enabled: page.backend && page.backend.totalCount > 0
            onClicked: page.backend.clear()
        }
    }
    RowLayout {
        Layout.fillWidth: true
        Text { Layout.fillWidth: true; text: "Live session"; color: page.theme.muted; font.pixelSize: 12 }
        CheckBox {
            objectName: "logAutoScrollControl"
            text: "Auto-scroll"; checked: page.autoScroll
            onToggled: { page.autoScroll = checked; page.followTail() }
            HoverHandler { cursorShape: Qt.PointingHandCursor }
            indicator: Rectangle {
                implicitWidth: 28; implicitHeight: 16; radius: 8
                x: 0; y: (parent.height-height)/2
                color: parent.checked ? page.theme.selected : page.theme.field
                border.color: page.theme.border
                Rectangle { width: 12; height: 12; radius: 6; x: parent.parent.checked ? 14 : 2; y: 2; color: page.theme.text }
            }
            contentItem: Text { text: parent.text; color: page.theme.text; font.pixelSize: 13; leftPadding: 36; verticalAlignment: Text.AlignVCenter }
        }
    }
    Rectangle {
        Layout.fillWidth: true; Layout.fillHeight: true; Layout.minimumHeight: 160
        radius: 8; color: page.theme.background; border.color: page.theme.border
        ListView {
            id: logList
            objectName: "logsList"
            anchors.fill: parent; anchors.margins: 8
            clip: true; spacing: 6
            model: page.backend
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}
            delegate: RowLayout {
                required property string timestamp
                required property string level
                required property string category
                required property string message
                width: logList.width - 12; spacing: 8
                readonly property color levelColor: level === "error" ? page.theme.danger : level === "warning" ? page.theme.warning : level === "debug" ? page.theme.muted : "#69cda0"
                Text {
                    Layout.preferredWidth: 64; Layout.alignment: Qt.AlignTop
                    text: timestamp; color: page.theme.muted; font.pixelSize: 11
                    font.family: Qt.platform.os === "osx" ? "Menlo" : "monospace"
                }
                Rectangle {
                    Layout.preferredWidth: 54; Layout.preferredHeight: 18; Layout.alignment: Qt.AlignTop
                    radius: 3; color: Qt.rgba(parent.levelColor.r,parent.levelColor.g,parent.levelColor.b,.14)
                    Text { anchors.centerIn: parent; text: level.toUpperCase(); color: parent.parent.levelColor; font.pixelSize: 10; font.bold: true }
                }
                TextEdit {
                    Layout.fillWidth: true; Layout.alignment: Qt.AlignTop
                    text: category + " · " + message
                    textFormat: TextEdit.PlainText; wrapMode: TextEdit.Wrap
                    readOnly: true; selectByMouse: true
                    color: page.theme.text; selectionColor: page.theme.selected
                    font.pixelSize: 12; font.family: Qt.platform.os === "osx" ? "Menlo" : "monospace"
                    Accessible.name: timestamp + " " + level + " " + category + " " + message
                }
            }
            Text {
                anchors.centerIn: parent
                width: parent.width; horizontalAlignment: Text.AlignHCenter
                text: page.backend && page.backend.totalCount > 0 ? "No logs match your filters." : "No logs in this session."
                visible: logList.count === 0; color: page.theme.muted; wrapMode: Text.Wrap
            }
            onCountChanged: page.followTail()
        }
    }
    Text {
        objectName: "logEntryCount"
        Layout.fillWidth: true
        text: "Showing " + (page.backend ? page.backend.count : 0) + " of " + (page.backend ? page.backend.totalCount : 0) + " entries (1,000 max)"
        color: page.theme.muted; font.pixelSize: 12
    }
    Connections { target: page.backend; function onChanged() { page.followTail() } }
}
