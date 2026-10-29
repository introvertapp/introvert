import QtQuick
import QtQuick.Controls
ComboBox {
    id: control
    HoverHandler { cursorShape: Qt.PointingHandCursor }
    implicitHeight: 34
    readonly property real widestOption: {
        var widest=0
        for(var i=0;i<count;++i) widest=Math.max(widest,optionMetrics.advanceWidth(textAt(i)))
        return Math.ceil(widest)
    }
    FontMetrics { id: optionMetrics; font.family: control.font.family; font.pixelSize: 15; font.weight: control.font.weight }
    font.pixelSize: 14
    font.weight: Font.Medium
    leftPadding: 9; rightPadding: 22
    contentItem: Text { text: control.displayText; color: control.palette.text; font: control.font; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight }
    indicator: Canvas {
        x: control.width-width-9; y: (control.height-height)/2; width: 12; height: 12
        onPaint: { var c=getContext("2d"); c.reset(); c.strokeStyle=control.palette.text; c.lineWidth=1.7; c.lineCap="round"; c.lineJoin="round"; c.beginPath(); c.moveTo(2,4); c.lineTo(6,8); c.lineTo(10,4); c.stroke() }
        Connections { target: control; function onPaletteChanged() { control.indicator.requestPaint() } }
    }
    background: Rectangle { implicitWidth: 120; implicitHeight: 34; color: control.palette.base; border.color: control.activeFocus ? control.palette.highlight : control.palette.mid; border.width: control.activeFocus ? 2 : 1; radius: 6 }
    delegate: ItemDelegate {
        HoverHandler { cursorShape: Qt.PointingHandCursor }
        id: option
        required property int index
        required property var modelData
        width: control.popup.availableWidth
        implicitHeight: 30; padding: 3; leftPadding: 24; hoverEnabled: true
        highlighted: control.highlightedIndex === index
        contentItem: Text { text: option.modelData; color: option.highlighted || option.hovered ? "white" : control.palette.text; font: optionMetrics.font; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight }
        background: Rectangle { radius: 4; color: option.highlighted || option.hovered ? control.palette.highlight : "transparent" }
        Text { x: 7; anchors.verticalCenter: parent.verticalCenter; text: option.index === control.currentIndex ? "✓" : ""; color: option.highlighted || option.hovered ? "white" : control.palette.text; font.pixelSize: 12 }
    }
    popup: Popup {
        objectName: "styledDropdownPopup"
        x: 0; y: control.height+4
        width: Math.max(control.width,control.widestOption+40)
        padding: 5
        implicitHeight: Math.min(contentItem.implicitHeight+10,320)
        background: Rectangle { color: control.palette.base; radius: 8; border.color: control.palette.mid }
        contentItem: ListView {
            clip: true; implicitHeight: contentHeight
            model: control.popup.visible ? control.delegateModel : null
            currentIndex: control.highlightedIndex
            ScrollIndicator.vertical: ScrollIndicator { visible: false }
        }
    }
}
