import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

SurfaceDialog {
    id: map
    parent: Overlay.overlay; anchors.centerIn: parent
    width: Math.min(760,parent.width-44); height: Math.min(600,parent.height-44)
    title: "Shortcuts"; modal: false; focus: true; padding: 12
    closePolicy: Popup.NoAutoClose
    showCloseButton: false
    background: Rectangle { color: "#f50f1216"; border.color: map.theme.border; radius: 8 }
    header: Item {
        implicitHeight: 44
        Text { anchors.left: parent.left; anchors.leftMargin: 12; anchors.verticalCenter: parent.verticalCenter; text: "Shortcuts"; font.pixelSize: 13; font.bold: true; color: map.theme.text }
        AppButton { anchors.right: parent.right; anchors.rightMargin: 8; anchors.verticalCenter: parent.verticalCenter; text: "×"; implicitWidth: 24; implicitHeight: 24; theme: map.theme; hint: "Close shortcuts"; onClicked: map.close() }
    }
    readonly property var sections: [
        {title:"PLAYBACK", rows:[["Space / K","Play or pause video preview"],["Left / Right","Move playhead by selected frame step"],["Shift + Left / Right","Move playhead by one frame"],["Home / End","Move playhead to start or end"],["Shift + Z","Reset timeline zoom to full view"]]},
        {title:"SEGMENTS", rows:[["R / Shift + R","Insert recap / move nearest edge to playhead"],["I / Shift + I","Insert intro / move nearest edge to playhead"],["C / Shift + C","Insert credits / move nearest edge to playhead"],["P / Shift + P","Insert preview / move nearest edge to playhead"],["Delete / Backspace","Delete selected segment drafts"],["Cmd/Ctrl + A","Select all segment drafts"],["Cmd/Ctrl + Z","Undo last timeline edit"]]},
        {title:"EDITOR", rows:[["S","Open or close segment drawer"],["H","Open or close this shortcut map"],["J / L","Go to previous or next episode"],["Cmd/Ctrl + S","Save timeline changes"],["Cmd/Ctrl + R","Reset timeline changes"],["Esc","Close shortcut map, then close video editor"]]}
    ]
    contentItem: ScrollView {
        ScrollBar.vertical.policy: ScrollBar.AlwaysOff
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        clip: true
        ColumnLayout {
            width: map.availableWidth; spacing: 20
            GridLayout {
                Layout.fillWidth: true; columns: map.width < 600 ? 1 : 2
                columnSpacing: 24; rowSpacing: 20
                Repeater {
                    model: map.sections
                    ColumnLayout {
                        required property var modelData
                        Layout.fillWidth: true; Layout.alignment: Qt.AlignTop; spacing: 7
                        Text { text: modelData.title; font.pixelSize: 11; font.bold: true; color: map.theme.muted }
                        Repeater {
                            model: modelData.rows
                            RowLayout {
                                required property var modelData
                                Layout.fillWidth: true; spacing: 10
                                Rectangle {
                                    Layout.preferredWidth: 145; implicitHeight: 20; radius: 3
                                    color: map.theme.field; border.color: map.theme.border
                                    Text { anchors.fill: parent; anchors.leftMargin: 5; verticalAlignment: Text.AlignVCenter; text: modelData[0]; font.family: Qt.platform.os === "osx" ? "Menlo" : "monospace"; font.pixelSize: 10; font.bold: true; color: map.theme.muted }
                                }
                                Text { Layout.fillWidth: true; text: modelData[1]; wrapMode: Text.WordWrap; color: map.theme.muted; font.pixelSize: 12 }
                            }
                        }
                    }
                }
            }
        }
    }
}
