import QtQuick
import QtQuick.Layouts

RowLayout {
    id: cards
    required property var theme
    property var entries: []
    property real gap: 10
    spacing: gap
    Repeater {
        model: cards.entries
        Rectangle {
            required property var modelData
            Layout.fillWidth: true; Layout.minimumWidth: 0
            implicitHeight: 67; radius: 8; color: cards.theme.card; border.color: cards.theme.border
            Column {
                anchors.fill: parent; anchors.margins: 14; spacing: 4
                Text { width: parent.width; text: modelData.label.toUpperCase(); elide: Text.ElideRight; font.pixelSize: 12; font.bold: true; color: cards.theme.muted }
                Text { width: parent.width; text: modelData.value === undefined || modelData.value === null || modelData.value === "—" || modelData.value === "-" ? "0" : String(modelData.value); elide: Text.ElideRight; font.pixelSize: 22; font.bold: true; color: cards.theme.text }
            }
        }
    }
}
