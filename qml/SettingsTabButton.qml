import QtQuick
import QtQuick.Controls
Button {
    id: tab
    HoverHandler { cursorShape: Qt.PointingHandCursor }
    required property var theme
    property string subtitle: ""
    property string symbol: ""
    property string iconName: ""
    property bool selected: false
    property real labelReveal: 1
    Behavior on labelReveal { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }
    Accessible.name: text
    height: 59
    leftPadding: 14; rightPadding: 14; topPadding: 10; bottomPadding: 10
    hoverEnabled: true
    background: Rectangle { radius: 8; color: tab.selected ? tab.theme.selected : tab.hovered ? tab.theme.hover : "transparent"; border.color: tab.selected ? tab.theme.accent : "transparent" }
    contentItem: Item {
        clip: true
        Rectangle { width: 32; height: 32; radius: 8; anchors.verticalCenter: parent.verticalCenter; color: tab.theme.field; IconGlyph { anchors.centerIn: parent; width: 18; height: 18; name: tab.iconName; color: tab.selected ? tab.theme.text : tab.theme.muted } }
        Column {
            objectName: "settingsTabLabels"
            anchors.verticalCenter: parent.verticalCenter; spacing: 3
            x: 42-(1-tab.labelReveal)*8; width: 170
            opacity: tab.labelReveal
            Text { text: tab.text; font.pixelSize: 15; font.bold: true; color: tab.theme.text }
            Text { width: parent.width; text: tab.subtitle; font.pixelSize: 12; color: tab.theme.muted; wrapMode: Text.WordWrap }
        }
    }
    AppToolTip { visible: tab.hovered && tab.labelReveal<0.2; text: tab.text; theme: tab.theme }
}
