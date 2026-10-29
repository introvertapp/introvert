import QtQuick
import QtQuick.Controls

Item {
    id: field
    required property var theme
    property string label: ""
    property string fieldKey: label
    property bool secretSaved: false
    signal valueEdited(var value)
    function clearSecret() { textEntry.text=""; reveal=false }
    property bool compact: false
    property string kind: "text"
    property string value: ""
    property bool checked: false
    property bool reveal: false
    onVisibleChanged: if(!visible) reveal=false
    property bool locked: kind === "url"
    implicitHeight: kind === "toggle" ? (compact ? 26 : 34) : (compact ? 62 : 64)
    implicitWidth: 500
    Text {
        anchors.left: parent.left; y: field.kind === "toggle" ? (field.compact ? 4 : 7) : 0
        text: field.label; font.pixelSize: 15; font.weight: Font.DemiBold; color: field.theme.text
    }
    AppTextField {
        id: textEntry; objectName: "input_"+field.fieldKey
        visible: field.kind !== "toggle" && field.kind !== "number"
        anchors.left: parent.left; anchors.right: parent.right
        anchors.rightMargin: field.kind === "secret" || field.kind === "url" ? 48 : 0
        y: field.compact ? 22 : 25; height: 40
        text: field.value; echoMode: field.kind === "secret" && !field.reveal ? TextInput.Password : TextInput.Normal
        readOnly: field.kind === "url" && field.locked
        inputMethodHints: field.kind === "secret" ? Qt.ImhSensitiveData | Qt.ImhNoPredictiveText : Qt.ImhNone
        placeholderText: ""
        onTextEdited: field.valueEdited(text)
        Accessible.name: field.label
    }
    AppButton {
        anchors.right: parent.right; y: field.compact ? 22 : 25
        visible: field.kind === "secret" || field.kind === "url"
        objectName: "fieldAction_"+field.label
        theme: field.theme; iconName: field.kind === "secret" ? (field.reveal ? "eye-off" : "eye") : (field.locked ? "lock" : "unlock")
        width: 40; height: 40; implicitWidth: 40
        hint: field.kind === "secret" ? "Show or hide typed value" : "Lock or unlock URL"
        onClicked: field.kind === "secret" ? field.reveal = !field.reveal : field.locked = !field.locked
    }
    AppNumberField {
        objectName: "number_"+field.label
        visible: field.kind === "number"
        anchors.left: parent.left; anchors.right: parent.right; y: field.compact ? 22 : 25; height: 40
        value: Number(field.value); onValueModified: field.valueEdited(value); Accessible.name: field.label
    }
    SegmentSwitch { objectName: "toggle_"+field.fieldKey; anchors.right: parent.right; y: field.compact ? 0 : 3; visible: field.kind === "toggle"; checked: field.checked; onToggled: field.valueEdited(checked); Accessible.name: field.label }
}
