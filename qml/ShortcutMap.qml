import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

SurfaceDialog {
    id: map
    parent: Overlay.overlay; anchors.centerIn: parent
    width: Math.min(960,parent.width-44); height: Math.min(760,parent.height-44)
    title: "Shortcuts"; modal: false; focus: true; padding: 20
    closePolicy: Popup.NoAutoClose
    showCloseButton: false
    readonly property real pinnedHeaderHeight: header.height+topPadding+37
    function shortcutText(keys, platform) {
        var mac=(platform || Qt.platform.os)==="osx"
        return keys.replace(/Cmd\/Ctrl/g,mac ? "⌘" : "Ctrl").replace(/Shift/g,mac ? "⇧" : "Shift")
    }
    background: Rectangle { color: map.theme.background; border.color: map.theme.border; radius: 8 }
    header: Item {
        implicitHeight: 58; z: 2
        HeaderGlass {
            objectName: "shortcutHeaderGlass"
            anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
            height: map.pinnedHeaderHeight
            theme: map.theme; backdropSource: tableScroll
            backdropRect: Qt.rect(0,0,width,height)
        }
        Text { anchors.left: parent.left; anchors.leftMargin: 20; anchors.verticalCenter: parent.verticalCenter; text: "Keyboard shortcuts"; font.pixelSize: 20; font.bold: true; color: map.theme.text }
        AppButton { objectName: "editorHelpCloseControl"; anchors.right: parent.right; anchors.rightMargin: 16; anchors.verticalCenter: parent.verticalCenter; glyphOnly: true; fontSize: 22; text: "×"; implicitWidth: 26; implicitHeight: 26; theme: map.theme; hint: "Close shortcuts"; onClicked: map.close() }
        RowLayout {
            anchors.left: parent.left; anchors.right: parent.right
            anchors.leftMargin: map.leftPadding+14; anchors.rightMargin: map.rightPadding+14
            y: parent.height+map.topPadding; height: 36; spacing: 20
            Text { Layout.fillWidth: true; text: "Description"; font.pixelSize: 13; font.bold: true; color: map.theme.muted }
            Text { Layout.preferredWidth: 260; text: "Shortcut"; font.pixelSize: 13; font.bold: true; color: map.theme.muted }
        }
        Rectangle {
            anchors.left: parent.left; anchors.right: parent.right; anchors.leftMargin: map.leftPadding; anchors.rightMargin: map.rightPadding
            y: map.pinnedHeaderHeight-1; height: 1; color: map.theme.border
        }
    }
    readonly property var sections: [
        {title:"PLAYBACK", rows:[["Space / K","Play or pause video preview"],["Left / Right","Move playhead by selected frame step"],["Shift + Left / Right","Move playhead by one frame"],["Home / End","Move playhead to start or end"],["Shift + Z","Reset timeline zoom to full view"]]},
        {title:"SEGMENTS", rows:[["R / Shift + R","Insert recap / move nearest edge to playhead"],["I / Shift + I","Insert intro / move nearest edge to playhead"],["C / Shift + C","Insert credits / move nearest edge to playhead"],["P / Shift + P","Insert preview / move nearest edge to playhead"],["Delete / Backspace","Delete selected segment drafts"],["Cmd/Ctrl + A","Select all segment drafts"],["Cmd/Ctrl + Z","Undo last timeline edit"]]},
        {title:"EDITOR", rows:[["S","Open or close segment drawer"],["E","Open or close episode drawer"],["H","Open or close this shortcut map"],["J / L","Go to previous or next episode"],["Cmd/Ctrl + S","Save timeline changes"],["Cmd/Ctrl + R","Reset timeline changes"],["Esc","Close shortcut map, then close video editor"]]}
    ]
    contentItem: Item {
        ScrollView {
            id: tableScroll
            objectName: "shortcutTableScroll"
            anchors.fill: parent
            anchors.topMargin: -(map.header.height+map.topPadding)
            anchors.leftMargin: -map.leftPadding; anchors.rightMargin: -map.rightPadding
            contentWidth: availableWidth; leftPadding: map.leftPadding; rightPadding: map.rightPadding
            ScrollBar.vertical.policy: ScrollBar.AlwaysOff
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            clip: true
            Column {
                width: tableScroll.availableWidth
                // Reserve the pinned header initially; rows scroll behind its glass.
                Item { width: parent.width; height: map.pinnedHeaderHeight }
                Repeater {
                    model: map.sections
                    Column {
                        id: section
                        required property var modelData
                        width: parent.width
                        Rectangle {
                            width: parent.width; height: 38; color: map.theme.field
                            Text { anchors.left: parent.left; anchors.leftMargin: 14; anchors.verticalCenter: parent.verticalCenter; text: section.modelData.title; font.pixelSize: 13; font.bold: true; font.letterSpacing: 1; color: map.theme.muted }
                        }
                        Repeater {
                            model: section.modelData.rows
                            Rectangle {
                                id: shortcutRow
                                required property var modelData
                                width: section.width; height: Math.max(48,description.implicitHeight+20)
                                color: map.theme.card
                                RowLayout {
                                    anchors.fill: parent; anchors.leftMargin: 14; anchors.rightMargin: 14; spacing: 20
                                    Text { id: description; Layout.fillWidth: true; text: shortcutRow.modelData[1]; wrapMode: Text.WordWrap; color: map.theme.text; font.pixelSize: 16 }
                                    Row {
                                        Layout.preferredWidth: 260; spacing: 6
                                        Repeater {
                                            model: map.shortcutText(shortcutRow.modelData[0]).split(" ")
                                            Rectangle {
                                                required property string modelData
                                                readonly property bool separator: modelData==="+" || modelData==="/"
                                                implicitWidth: separator ? keyLabel.implicitWidth+2 : Math.max(32,keyLabel.implicitWidth+20)
                                                implicitHeight: 30; radius: 4
                                                color: separator ? "transparent" : map.theme.field
                                                border.width: separator ? 0 : 1; border.color: map.theme.selected
                                                Text {
                                                    id: keyLabel
                                                    anchors.centerIn: parent; text: parent.modelData
                                                    font.pixelSize: text==="⌘" || text==="⇧" ? 20 : 16; font.weight: Font.DemiBold
                                                    color: parent.separator ? map.theme.muted : map.theme.text
                                                }
                                            }
                                        }
                                    }
                                }
                                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: map.theme.border }
                            }
                        }
                    }
                }
            }
        }
    }
}
