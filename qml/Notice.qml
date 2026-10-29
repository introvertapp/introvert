import QtQuick
import QtQuick.Layouts

Rectangle {
    id: notice
    required property var theme
    property string text: ""
    property bool warning: false
    implicitHeight: label.implicitHeight + 20
    color: theme.card; radius: 8; border.color: warning ? theme.warning : theme.border
    Text { id: label; x: 12; y: 10; width: parent.width - 24; text: notice.text; wrapMode: Text.WordWrap; font.pixelSize: 14; color: notice.warning ? notice.theme.warning : notice.theme.muted }
}
