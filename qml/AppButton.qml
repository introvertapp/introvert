import QtQuick
import QtQuick.Controls

Button {
    id: control
    property var theme
    property real progress: -1
    property bool glyphOnly: false
    property bool subtle: false
    property int iconSize: 18
    property real iconOpacity: 1
    property bool selected: false
    property bool outlineSelection: false
    property bool primary: false
    property bool destructive: false
    property string hint: ""
    property int fontSize: 14
    property string iconName: ""
    hoverEnabled: true
    implicitHeight: 34
    implicitWidth: Math.max(32, (iconName ? 18 : label.implicitWidth) + 24)
    padding: 0
    leftPadding: Math.min(10, Math.max(0,(width-(iconName ? 18 : label.implicitWidth))/2))
    rightPadding: leftPadding
    topPadding: Math.max(0,(height-18)/2)
    bottomPadding: topPadding
    opacity: enabled ? 1 : 0.45
    Accessible.name: hint || text
    HoverHandler { id: hintHover; cursorShape: Qt.PointingHandCursor }
    AppToolTip { visible: hintHover.hovered && control.hint !== ""; text: control.hint; theme: control.theme }

    background: Rectangle {
        visible: !control.glyphOnly
        radius: control.subtle ? 4 : 8
        color: control.subtle ? (control.hovered ? "#12ffffff" : "#0affffff") : control.primary ? "#b5412f" : control.selected && !control.outlineSelection ? control.theme.selected : (control.hovered ? control.theme.hover : control.theme.card)
        Canvas {
            id: progressFill
            objectName: "buttonProgressFill"
            anchors.fill: parent; visible: control.progress>=0; opacity: .65
            antialiasing: true
            onAvailableChanged: if(available) requestPaint()
            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()
            Connections { target: control; function onProgressChanged() {progressFill.requestPaint()} }
            onPaint: {
                var c=getContext("2d");c.reset();var r=7,w=width-2,h=height-2
                c.beginPath();c.moveTo(1+r,1);c.lineTo(1+w-r,1);c.arcTo(1+w,1,1+w,1+r,r)
                c.lineTo(1+w,1+h-r);c.arcTo(1+w,1+h,1+w-r,1+h,r)
                c.lineTo(1+r,1+h);c.arcTo(1,1+h,1,1+h-r,r)
                c.lineTo(1,1+r);c.arcTo(1,1,1+r,1,r);c.closePath();c.clip()
                c.fillStyle="#33d17a";c.fillRect(1,1,w*Math.max(0,Math.min(1,control.progress)),h)
            }
        }
        border.width: control.subtle ? 0 : control.activeFocus ? 2 : 1
        border.color: control.destructive ? control.theme.danger : control.activeFocus || control.selected ? control.theme.accent : control.theme.border
    }

    contentItem: Item {
        implicitHeight: 18
        Text {
        id: label
        anchors.fill: parent
        visible: control.iconName === ""
        text: control.text
        color: control.primary ? "white" : control.subtle ? (control.hovered ? "#c7ffffff" : "#7affffff") : control.theme.text
        font.pixelSize: control.fontSize
        font.weight: control.subtle ? Font.Bold : control.selected ? Font.Bold : Font.DemiBold
        font.family: control.subtle ? (Qt.platform.os === "osx" ? "Menlo" : Qt.platform.os === "windows" ? "Consolas" : "monospace") : control.font.family
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
        }
        AppImage { anchors.centerIn: parent; width: control.iconSize; height: control.iconSize; opacity: control.iconOpacity; visible: control.iconName !== "" && source.toString() !== ""; source: ["eye","eye-off","lock","unlock","clapboard","play","keyboard","volume","volume-muted","pause","reset","save","chevron-left","chevron-right"].indexOf(control.iconName)>=0 ? "icons/"+control.iconName+".svg" : "" }
        IconGlyph { anchors.centerIn: parent; width: control.iconSize; height: control.iconSize; opacity: control.iconOpacity; visible: control.iconName !== "" && ["eye","eye-off","lock","unlock","clapboard","play","keyboard","volume","volume-muted","pause","reset","save","chevron-left","chevron-right"].indexOf(control.iconName)<0; name: control.iconName; color: control.theme.text }
    }
}
