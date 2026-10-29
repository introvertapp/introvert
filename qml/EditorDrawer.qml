import QtQuick
import QtQuick.Controls

Item {
    id: drawer
    required property var editor
    required property var theme
    property real reveal: 1
    clip: true
    readonly property real tableWidth: 218
    function columnWidth(index) { return tableWidth*[54,64,64,24][index]/206 }
    Column {
        opacity: drawer.reveal
        enabled: editor.drawerOpen
        x: 9; width: drawer.tableWidth
        Row {
            height: 22
            Repeater {
                model: ["Type","Start","End",""]
                Text { required property int index; required property string modelData; width: drawer.columnWidth(index); height: 22; leftPadding: 5; text: modelData; color: "#59ffffff"; font.pixelSize: 11; font.weight: Font.DemiBold; verticalAlignment: Text.AlignVCenter }
            }
        }
        Repeater {
            model: ["Recap","Intro","Credits","Preview"]
            Item {
                id: entry
                required property int index
                required property string modelData
                width: drawer.tableWidth; height: 25
                Rectangle { anchors.fill: parent; color: editor.selectedSegment===entry.index ? "#14ffd12a" : "transparent" }
                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: "#0effffff" }
                Row {
                    anchors.fill: parent
                    AbstractButton {
                        HoverHandler { cursorShape: Qt.PointingHandCursor }
                        width: drawer.columnWidth(0); height: 25; padding: 0; enabled: editor.segmentValid(entry.index)
                        onClicked: { editor.selectedSegment=entry.index;editor.selectedIndices=[entry.index];var value=editor.segments[entry.index];if(value)editor.cursorSeconds=entry.index>1?value.end:value.start }
                        contentItem: Text { text: entry.modelData; leftPadding: 5; verticalAlignment: Text.AlignVCenter; font.pixelSize: 12; font.weight: Font.DemiBold; color: "#adffffff" }
                    }
                    Repeater {
                        model: ["start","end"]
                        Item {
                            required property string modelData
                            required property int index
                            width: drawer.columnWidth(index+1); height: 25
                            TextField {
                                objectName: "drawer_" + entry.modelData + "_" + modelData
                                x: 5; y: 3; width: parent.width-10; height: 18
                                leftPadding: 3; rightPadding: 3; topPadding: 1; bottomPadding: 1
                                font.pixelSize: 12; color: "#bdffffff"; selectByMouse: true
                                text: editor.draftText(entry.index,modelData)
                                readOnly: !editor.editable
                                Accessible.name: entry.modelData + " " + modelData
                                onActiveFocusChanged: { editor.fieldEditing=activeFocus;if(activeFocus)editor.remember() }
                                onTextEdited: editor.updateTableDraft(entry.index,modelData,text)
                                background: Rectangle { radius: 3; color: parent.activeFocus ? "#0f0f12" : "#131315"; border.color: parent.activeFocus ? "#38ffffff" : "transparent" }
                            }
                        }
                    }
                    AbstractButton {
                        HoverHandler { cursorShape: Qt.PointingHandCursor }
                        id: insertControl
                        objectName: "insert_"+entry.modelData
                        Accessible.name: "Insert "+entry.modelData+" at playhead"
                        AppToolTip { visible: insertControl.hovered; text: "Insert "+entry.modelData+" at playhead ("+["R","I","C","P"][entry.index]+")"; theme: drawer.theme }
                        hoverEnabled: true
                        width: drawer.columnWidth(3); height: 25
                        visible: !editor.segmentValid(entry.index)
                        enabled: editor.editable
                        onClicked: editor.insertDraft(entry.index)
                        contentItem: Item { AppImage { anchors.centerIn: parent; width: 14; height: 14; source: "icons/insert-timeline.svg"; opacity: insertControl.hovered ? .72 : .34 } }
                    }
                }
            }
        }
    }
}
