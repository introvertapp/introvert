import QtQuick
import QtQuick.Controls
CheckBox {
    id: control
    HoverHandler { cursorShape: Qt.PointingHandCursor }
    implicitHeight: 26
    leftPadding: 0; rightPadding: 5
    spacing: 7
    opacity: enabled ? 1 : 0.4
    indicator: Rectangle {
        x: 0; y: (control.height-height)/2; width: 15; height: 15; radius: 3
        color: control.checked ? control.palette.highlight : control.palette.base
        border.color: control.activeFocus ? control.palette.highlight : control.palette.mid
        Text { anchors.centerIn: parent; text: control.checked ? "✓" : ""; color: control.palette.text; font.pixelSize: 12; font.bold: true }
    }
    contentItem: Text { text: control.text; leftPadding: 22; verticalAlignment: Text.AlignVCenter; color: control.palette.text; font.pixelSize: 12 }
}
