import QtQuick
import QtQuick.Controls
SpinBox {
    id: control
    implicitWidth: 92; implicitHeight: 40
    from: -2147483647; to: 2147483647
    editable: true
    textFromValue: function(value, locale) { return String(value) }
    valueFromText: function(text, locale) { return Number(text) }
    leftPadding: 12; rightPadding: 30; topPadding: 8; bottomPadding: 8
    contentItem: TextInput {
        text: control.textFromValue(control.value,control.locale)
        font.pixelSize: 15; color: control.palette.text
        verticalAlignment: TextInput.AlignVCenter
        readOnly: !control.editable; selectByMouse: true
        validator: control.validator
        inputMethodHints: Qt.ImhFormattedNumbersOnly
        selectionColor: control.palette.highlight; selectedTextColor: control.palette.highlightedText
    }
    background: Rectangle { color: control.palette.base; radius: 8; border.color: control.activeFocus ? control.palette.highlight : control.palette.mid
        Rectangle { x: parent.width-26; anchors.verticalCenter: parent.verticalCenter; width: 14; height: 18; radius: 5; color: "#0cffffff" }
    }
    up.indicator: Item {
        HoverHandler { cursorShape: Qt.PointingHandCursor }
        objectName: control.objectName+"_up"
        x: control.width-28; y: 11; width: 18; height: 9
        AppImage { anchors.centerIn: parent; width: 9; height: 9; source: "icons/chevron-right.svg"; rotation: -90; opacity: control.up.hovered ? 1 : .7 }
    }
    down.indicator: Item {
        HoverHandler { cursorShape: Qt.PointingHandCursor }
        objectName: control.objectName+"_down"
        x: control.width-28; y: 20; width: 18; height: 9
        AppImage { anchors.centerIn: parent; width: 9; height: 9; source: "icons/chevron-right.svg"; rotation: 90; opacity: control.down.hovered ? 1 : .7 }
    }
}
