import QtQuick
import QtQuick.Controls

Dialog {
    id: dialog
    required property var theme
    property bool showCloseButton: true
    padding: 20
    standardButtons: Dialog.NoButton
    background: Rectangle { color: dialog.theme.card; border.color: dialog.theme.border; radius: 14 }
    header: Text { text: dialog.title; padding: 20; bottomPadding: 8; font.pixelSize: 18; font.bold: true; color: dialog.theme.text; wrapMode: Text.WordWrap }
    footer: Item { implicitHeight: dialog.showCloseButton ? 54 : 0; visible: dialog.showCloseButton; AppButton { anchors.right: parent.right; anchors.rightMargin: 16; anchors.verticalCenter: parent.verticalCenter; text: "Close"; theme: dialog.theme; onClicked: dialog.close() } }
}
