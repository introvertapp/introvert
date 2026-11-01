import QtQuick
import QtQuick.Controls
TextField {
    id: control
    property bool clearButtonEnabled: false
    implicitHeight: 34
    leftPadding: 10; rightPadding: clearButton.visible ? clearButton.width + 8 : 10
    font.pixelSize: 14
    font.weight: Font.Medium
    color: palette.text
    placeholderTextColor: palette.placeholderText
    background: Rectangle { color: control.palette.base; border.color: control.palette.mid; border.width: 1; radius: 6 }
    AbstractButton {
        id: clearButton
        objectName: control.objectName + "ClearButton"
        anchors.right: parent.right; anchors.rightMargin: 4
        anchors.verticalCenter: parent.verticalCenter
        width: 26; height: Math.min(26, control.height - 4)
        visible: control.clearButtonEnabled && control.text.length > 0
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
