import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "SegmentTime.js" as SegmentTime

Rectangle {
    id: row
    required property var theme
    required property var episode
    property string timestampFormat: "both"
    property var submissionState: ({})
    property string scanState: ""
    function statusLabel(value) {return ({OK:"Submitted",DUPLICATE:"Exists",FAIL:"Failed","RATE LIMITED":"Limited",PROCESSING:"Submitting",WAITING:"Queued",SKIP:"Skipped"})[value] || value}
    function comparisonLabel(value) {return ({MISSING:"New",DUPLICATE:"Match",DIFFERENT:"Different"})[value] || value}
    function statusColor(value) {return value==="FAIL" ? "#ff6b6b" : value==="RATE LIMITED" ? "#ffb833" : value==="OK" ? "#00d1cd" : value==="DUPLICATE" ? "#9aa7ff" : theme.text}
    function valuesFor(kind) {
        var raw=episode.segmentValues && episode.segmentValues[kind.toLowerCase()]
        if(raw)return raw
        var ends=valueFor(kind).split(/\s*[–→]\s*/)
        function parse(value) {var parts=value.split(":");return parts.reduce(function(total,part){return total*60+Number(part)},0)}
        return ends.length===2 ? [parse(ends[0]),parse(ends[1])] : null
    }
    function timestamp(kind) {var raw=valuesFor(kind);return raw ? SegmentTime.lines(raw[0],raw[1],timestampFormat) : ""}
    property bool preview: false
    property var segmentManager: null
    readonly property string segmentError: segmentManager && segmentManager.episodeId === episode.id ? segmentManager.message : ""
    property string editingKind: ""
    readonly property bool editingAllowed: preview || Boolean(segmentManager && segmentManager.editingAllowed)
    onEditingAllowedChanged: if(!editingAllowed)editingKind=""
    property var selectedKinds: []
    onSelectedKindsChanged: selectionChanged()
    signal editorRequested()
    signal actionRequested(string title)
    signal submissionErrorRequested(string title, string message)
    signal selectionChanged()
    function valueFor(kind) { return episode[kind.toLowerCase()] || "—" }
    function available(kind) { return valueFor(kind) !== "—" }
    function duration(value) {
        var ends = value.split(/\s*[–→]\s*/)
        function seconds(text) { var parts=text.split(":"); return parts.length===2 ? Number(parts[0])*60+Number(parts[1]) : Number(text) }
        return ends.length===2 ? " (" + Math.round((seconds(ends[1])-seconds(ends[0]))*10)/10 + "s)" : ""
    }
    objectName: "episode_" + episode.id
    color: hover.hovered ? "#161619" : theme.card
    border.color: theme.border; radius: 12
    implicitHeight: content.implicitHeight + 28
    HoverHandler { id: hover }
    ColumnLayout {
        id: content
        anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 14; anchors.rightMargin: statusPanel.visible ? statusPanel.width+26 : 14
        spacing: 18
        RowLayout {
            Layout.fillWidth: true; spacing: 8; Layout.preferredHeight: 24
            Text {
                text: row.episode.number + ". " + row.episode.title
                color: row.theme.text; font.pixelSize: 17; font.weight: Font.Bold
                elide: Text.ElideRight; Layout.maximumWidth: row.width-160
            }
            AppButton {
                objectName: "openEditor_" + row.episode.id
                text: "▻"; hint: "Open video editor"; iconName: "clapboard"
                implicitWidth: 26; implicitHeight: 24; padding: 4; theme: row.theme
                onClicked: row.editorRequested()
            }
            Item { Layout.fillWidth: true }
            Text { visible: row.episode.status === "Edited"; text: "Edited"; color: "#ffb833"; font.pixelSize: 12 }
        }
        ColumnLayout {
            spacing: 10; Layout.fillWidth: true
            Repeater {
                model: ["Recap", "Intro", "Credits", "Preview"]
                ColumnLayout {
                    id: segmentLine
                    objectName: "segmentLine_" + row.episode.id + "_" + modelData
                    required property string modelData
                    property color segmentColor: modelData === "Intro" ? "#22b8bd" : modelData === "Credits" ? "#aaa8ff" : modelData === "Recap" ? "#dbaa69" : "#dc88ca"
                    Layout.fillWidth: true; spacing: 0
                    RowLayout {
                        spacing: 10; Layout.preferredHeight: row.available(segmentLine.modelData) && row.timestampFormat === "both" ? 30 : 18
                        SegmentToggle {
                            text: ""; implicitWidth: 16; implicitHeight: 18
                            enabled: row.available(segmentLine.modelData)
                            objectName: "select_" + row.episode.id + "_" + segmentLine.modelData
                            Accessible.name: segmentLine.modelData + " selected for submission"
                            checked: row.selectedKinds.indexOf(segmentLine.modelData) >= 0
                            onToggled: {
                                var kinds=row.selectedKinds.slice(); var i=kinds.indexOf(segmentLine.modelData)
                                if(checked && i<0) kinds.push(segmentLine.modelData)
                                if(!checked && i>=0) kinds.splice(i,1)
                                row.selectedKinds=kinds; row.selectionChanged()
                            }
                        }
                        AbstractButton {
                            HoverHandler { cursorShape: Qt.PointingHandCursor }
                            objectName: "editSegment_"+row.episode.id+"_"+segmentLine.modelData
                            enabled: row.editingAllowed
                            visible: row.editingKind !== segmentLine.modelData
                            implicitWidth: timestamp.implicitWidth + (row.available(segmentLine.modelData) ? 0 : 16)
                            implicitHeight: timestamp.implicitHeight; hoverEnabled: true
                            onClicked: { var ends=row.valueFor(segmentLine.modelData).split(/\s*[–→]\s*/); var raw=row.valuesFor(segmentLine.modelData); startField.text=raw?(row.timestampFormat==="seconds"?SegmentTime.seconds(raw[0]):SegmentTime.mmss(raw[0])):""; endField.text=raw?(row.timestampFormat==="seconds"?SegmentTime.seconds(raw[1]):SegmentTime.mmss(raw[1])):""; row.editingKind=segmentLine.modelData }
                            Accessible.name: row.available(segmentLine.modelData) ? segmentLine.modelData + ": " + row.valueFor(segmentLine.modelData) : "No " + segmentLine.modelData.toLowerCase()
                            background: Rectangle { visible: !row.available(segmentLine.modelData); color: row.theme.field; radius: 10; border.color: row.theme.border }
                            contentItem: Row {
                                id: timestamp
                                spacing: 6
                                Item { visible: row.available(segmentLine.modelData); width: 20; height: 14; anchors.verticalCenter: parent.verticalCenter; IconGlyph { objectName: "segmentClock_"+row.episode.id+"_"+segmentLine.modelData; name: "clock"; antialiasing: true; width: 14; height: 14; color: segmentLine.segmentColor; opacity: .8 } }
                                Text { text: row.available(segmentLine.modelData) ? segmentLine.modelData+":" : "No "+segmentLine.modelData.toLowerCase(); width: row.available(segmentLine.modelData) ? 46 : implicitWidth; color: row.theme.muted; font.pixelSize: 12; anchors.verticalCenter: parent.verticalCenter }
                                Text { objectName: "timestamp_"+row.episode.id+"_"+segmentLine.modelData; visible: row.available(segmentLine.modelData); text: row.timestamp(segmentLine.modelData); color: segmentLine.segmentColor; font.pixelSize: 12; font.weight: Font.DemiBold; lineHeight: 1.2 }
                            }
                        }
                        AppImage {
                            source: "icons/clapboard.svg"; visible: row.episode.status === "Edited" && row.available(segmentLine.modelData)
                            Layout.preferredWidth: 14; Layout.preferredHeight: 14
                            Layout.minimumWidth: Layout.preferredWidth; Layout.maximumWidth: Layout.preferredWidth
                            Layout.minimumHeight: Layout.preferredHeight; Layout.maximumHeight: Layout.preferredHeight
                        }
                        Repeater {
                            model: row.submissionState.badges ? (row.submissionState.badges[segmentLine.modelData.toLowerCase()] || []).filter(function(badge){var times=row.valuesFor(segmentLine.modelData);return times && times[0]===badge.start && times[1]===badge.end}).map(function(badge){return badge.endpoint}) : []
                            AppImage {
                                required property string modelData
                                Layout.preferredWidth: modelData==="theintrodb_org" ? 34 : 16; Layout.preferredHeight: modelData==="theintrodb_org" ? 8 : 13
                                Layout.minimumWidth: Layout.preferredWidth; Layout.maximumWidth: Layout.preferredWidth
                                Layout.minimumHeight: Layout.preferredHeight; Layout.maximumHeight: Layout.preferredHeight
                                fillMode: Image.PreserveAspectFit; source: "assets/"+(modelData==="theintrodb_org" ? "theintrodb_org_logo.svg" : "introdb_app_logo.svg")
                            }
                        }
                        Item { Layout.fillWidth: true }
                    }
                    Rectangle {
                        visible: row.editingKind === segmentLine.modelData
                        enabled: row.editingAllowed
                        Layout.leftMargin: 26; Layout.preferredWidth: Math.min(520,row.width-70)
                        implicitHeight: row.segmentError ? 94 : 72; color: row.theme.field; border.color: row.theme.border; radius: 12
                        ColumnLayout {
                            anchors.fill: parent; anchors.margins: 10; spacing: 6
                            RowLayout {
                                Text { text: segmentLine.modelData; color: segmentLine.segmentColor; font.pixelSize: 12; Layout.preferredWidth: 42 }
                                AppTextField { id: startField; objectName: "segmentStart_"+row.episode.id+"_"+segmentLine.modelData; Layout.preferredWidth: 90; implicitHeight: 28; placeholderText: "Start"; Accessible.name: "Segment start" }
                                AppTextField { id: endField; objectName: "segmentEnd_"+row.episode.id+"_"+segmentLine.modelData; Layout.preferredWidth: 90; implicitHeight: 28; placeholderText: "End"; Accessible.name: "Segment end" }
                                AppButton { objectName: "saveSegment_"+row.episode.id+"_"+segmentLine.modelData; text: "Save"; implicitHeight: 28; theme: row.theme; enabled: Boolean(row.segmentManager) && !row.segmentManager.busy; onClicked: row.segmentManager.saveInline(row.episode.id,segmentLine.modelData,startField.text,endField.text) }
                                AppButton { objectName: "deleteSegment_"+row.episode.id+"_"+segmentLine.modelData; visible: row.available(segmentLine.modelData); text: "Delete"; implicitHeight: 28; theme: row.theme; enabled: Boolean(row.segmentManager) && !row.segmentManager.busy; onClicked: row.segmentManager.deleteInline(row.episode.id,segmentLine.modelData) }
                                AppButton { text: "Cancel"; implicitHeight: 28; theme: row.theme; onClicked: row.editingKind="" }
                            }
                            Text { visible: row.segmentError.length>0; text: row.segmentError; color: row.theme.warning; font.pixelSize: 12; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                            Text { text: "Format: seconds, MM:SS, or HH:MM:SS (decimals accepted)."; font.pixelSize: 12; color: row.theme.muted }
                        }
                    }
                }
            }
        }
    }
    Column {
        id: statusPanel
        objectName: "episodeStatus_"+row.episode.id
        anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 14
        width: Math.min(220,row.width*.24); spacing: 7
        visible: row.scanState!=="" || Object.keys(row.submissionState.segments || {}).length>0
        Text { width: parent.width; visible: text!==""; text: row.scanState; horizontalAlignment: Text.AlignRight; color: text.indexOf("NO ")===0 ? "#ffb833" : row.theme.text; font.pixelSize: 12; font.bold: true }
        Repeater {
            model: Object.keys(row.submissionState.segments || {}).map(function(kind){return row.submissionState.segments[kind]})
            Column {
                required property var modelData
                width: statusPanel.width; spacing: 3
                Text { objectName: "submitStatus_"+row.episode.id+"_"+modelData.kind; visible: modelData.result!=="FAIL"; width: parent.width; text: modelData.kind.charAt(0).toUpperCase()+modelData.kind.slice(1)+": "+row.statusLabel(modelData.result)+(modelData.comparison ? " · "+row.comparisonLabel(modelData.comparison) : ""); horizontalAlignment: Text.AlignRight; color: row.statusColor(modelData.result); font.pixelSize: 12; font.bold: true; wrapMode: Text.WordWrap }
                AppButton {
                    objectName: "submissionFailure_"+row.episode.id+"_"+modelData.kind
                    visible: modelData.result==="FAIL"; x: parent.width-width
                    text: "FAILED"; implicitWidth: 52; implicitHeight: 18; fontSize: 11; destructive: true; theme: row.theme
                    Accessible.name: modelData.kind+" submission failed: show error"
                    onClicked: {
                        var code=Number(modelData.httpStatus)
                        var message=modelData.error || "Submission failed. No error details were returned."
                        if(code>=400 && code<=599) message+="\n\nError: "+code
                        row.submissionErrorRequested(modelData.kind.charAt(0).toUpperCase()+modelData.kind.slice(1)+" submission failed",message)
                    }
                }
                Skeleton { visible: modelData.result==="PROCESSING"; width: parent.width; height: 5; theme: row.theme }
                Text { objectName: "submissionInlineError_"+row.episode.id+"_"+modelData.kind; visible: modelData.result!=="FAIL" && Boolean(modelData.error); width: parent.width; text: modelData.error || ""; horizontalAlignment: Text.AlignRight; color: row.statusColor(modelData.result); font.pixelSize: 12; wrapMode: Text.WordWrap }
            }
        }
    }

}
