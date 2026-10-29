import QtQuick
import QtQuick.Controls

Switch {
    id: control
    HoverHandler { cursorShape: Qt.PointingHandCursor }
    property bool compact: false
    property color activeColor: compact ? "#33d17a" : "#00c7c7"
    readonly property real trackWidth: compact ? 24 : 32
    readonly property real trackHeight: compact ? 13 : 18
    implicitWidth: compact ? 8 + trackWidth + 7 + label.implicitWidth + 8 : trackWidth
    implicitHeight: compact ? 24 : trackHeight
    padding: 0
    spacing: 0
    background: Rectangle {
        visible: control.compact
        radius: 5; color: control.checked ? "#18362c" : control.palette.button
        border.color: control.palette.mid
    }
    indicator: Rectangle {
        objectName: "switchTrack"
        x: control.compact ? 8 : 0; y: (control.height-height)/2
        width: control.trackWidth; height: control.trackHeight; radius: height/2
        color: control.checked ? control.activeColor : "#35353e"
        Rectangle {
            width: parent.height-4; height: width; radius: width/2
            x: control.checked ? parent.width-width-2 : 2; y: 2; color: "white"
        }
    }
    contentItem: Item {
        Text {
            id: label
            objectName: "switchLabel"
            visible: control.compact
            x: 8 + control.trackWidth + 7; height: parent.height
            text: control.text; verticalAlignment: Text.AlignVCenter
            color: control.palette.text; font.pixelSize: 13; font.weight: Font.DemiBold
        }
    }
}
