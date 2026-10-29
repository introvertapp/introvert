import QtQuick
import QtQuick.Controls
ToolTip {
    id: tip
    required property var theme
    delay: 550
    padding: 8
    implicitWidth: Math.min(360, contentItem.implicitWidth + 16)
    contentItem: Text { text: tip.text; color: tip.theme.text; font.pixelSize: 13; wrapMode: Text.WordWrap }
    background: Rectangle { color: tip.theme.card; border.color: tip.theme.border; radius: 6 }
}
