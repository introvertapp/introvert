import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: dialog
    required property var theme
    property string message: ""
    property string confirmText: "Close"
    signal confirmed()
    objectName: "actionDialog"
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(520, parent ? parent.width - 40 : 520)
    modal: true; focus: true; padding: 20
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    background: Rectangle { color: dialog.theme.card; border.color: dialog.theme.border; radius: 14 }
    header: Text { text: dialog.title; padding: 20; bottomPadding: 8; font.pixelSize: 18; font.bold: true; color: dialog.theme.text; wrapMode: Text.WordWrap }
    contentItem: ColumnLayout {
        spacing: 18
        ScrollView {
        ScrollBar.vertical.policy: ScrollBar.AlwaysOff
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            id: messageScroll
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(body.implicitHeight, Math.max(80, (dialog.parent ? dialog.parent.height : 700) - 200))
            clip: true; contentWidth: availableWidth
            Text { id: body; objectName: "dialogMessage"; width: messageScroll.availableWidth; text: dialog.message; textFormat: Text.PlainText; wrapMode: Text.WordWrap; color: dialog.theme.muted; font.pixelSize: 15 }
        }
        RowLayout {
            Layout.alignment: Qt.AlignRight
            AppButton { text: "Cancel"; visible: dialog.confirmText !== "Close"; theme: dialog.theme; onClicked: dialog.close() }
            AppButton { objectName: "dialogConfirmControl"; text: dialog.confirmText; theme: dialog.theme; onClicked: { dialog.confirmed(); dialog.close() } }
        }
    }
}
