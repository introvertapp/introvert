import QtQuick
import QtQuick.Controls
TextField {
    id: control
    implicitHeight: 34
    leftPadding: 10; rightPadding: 10
    font.pixelSize: 14
    font.weight: Font.Medium
    color: palette.text
    placeholderTextColor: palette.placeholderText
    background: Rectangle { color: control.palette.base; border.color: control.palette.mid; border.width: 1; radius: 6 }
}
