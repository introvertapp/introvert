import QtQuick
import QtQuick.Controls
TextField {
    id: control
    property bool clearButtonEnabled: false
    property bool searchShortcutEnabled: clearButtonEnabled
    readonly property string searchShortcutLabel: Qt.platform.os === "osx" ? "⌘K" : "Ctrl+K"
    readonly property bool searchEditing: control.activeFocus || clearButton.activeFocus
    implicitHeight: 34
    leftPadding: 10; rightPadding: clearButton.visible ? clearButton.width + 8 : shortcutHint.visible ? shortcutHint.implicitWidth + 20 : 10
    font.pixelSize: 14
    font.weight: Font.Medium
    color: palette.text
    placeholderTextColor: palette.placeholderText
    background: Rectangle { color: control.palette.base; border.color: control.palette.mid; border.width: 1; radius: 6 }
    Shortcut {
        // Qt maps Ctrl to Command on macOS and Control on Windows/Linux.
        sequence: "Ctrl+K"
        enabled: control.searchShortcutEnabled && control.enabled && control.visible
        onActivated: { control.forceActiveFocus(Qt.ShortcutFocusReason); control.selectAll() }
    }
    Text {
        id: shortcutHint
        objectName: control.objectName + "ShortcutHint"
        anchors.right: parent.right; anchors.rightMargin: 10
        anchors.verticalCenter: parent.verticalCenter
        visible: control.searchShortcutEnabled && !control.searchEditing
        text: control.searchShortcutLabel; color: control.placeholderTextColor; font.pixelSize: 12
    }
    AbstractButton {
        id: clearButton
        objectName: control.objectName + "ClearButton"
        anchors.right: parent.right; anchors.rightMargin: 4
        anchors.verticalCenter: parent.verticalCenter
        width: 26; height: Math.min(26, control.height - 4)
        visible: control.clearButtonEnabled && control.text.length > 0 && (!control.searchShortcutEnabled || control.searchEditing)
        Accessible.name: "Clear search"
        HoverHandler { cursorShape: Qt.PointingHandCursor }
        contentItem: Item {
            Rectangle {
                anchors.centerIn: parent; width: 12; height: 1.5; radius: .75
                rotation: 45; antialiasing: true; color: control.placeholderTextColor
            }
            Rectangle {
                anchors.centerIn: parent; width: 12; height: 1.5; radius: .75
                rotation: -45; antialiasing: true; color: control.placeholderTextColor
            }
        }
        background: Rectangle { color: clearButton.hovered || clearButton.activeFocus ? control.palette.button : "transparent"; radius: 4 }
        onClicked: { control.clear(); control.textEdited(); control.forceActiveFocus() }
    }
}
