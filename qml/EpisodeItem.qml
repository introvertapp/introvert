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
    readonly property var endpointStatuses: {
        var endpoints={}
        Object.keys(submissionState.segments || {}).forEach(function(key) {
            var state=submissionState.segments[key], endpoint=state.endpoint || "introdb.app"
            if(!endpoints[endpoint]) endpoints[endpoint]={endpoint:endpoint,name:endpoint==="theintrodb.org" ? "TheIntroDB" : "IntroDB",states:[],errors:[],cooldown:0}
            var group=endpoints[endpoint]
            group.states.push(state)
            if(state.result==="FAIL") {
                var message=state.error || "Submission failed. No error details were returned."
                var code=Number(state.httpStatus)
                if(code>=400 && code<=599) message+="\n\nError: "+code
                if(group.errors.indexOf(message)<0) group.errors.push(message)
            }
            group.cooldown=Math.max(group.cooldown,Number(state.cooldownSeconds || 0))
        })
        return ["theintrodb.org","introdb.app"].filter(function(endpoint){return Boolean(endpoints[endpoint])}).map(function(endpoint){
            var group=endpoints[endpoint]
            var processing=group.states.some(function(state){return state.result==="PROCESSING"})
            var pending=group.states.some(function(state){return ["WAITING","RATE LIMITED"].indexOf(state.result)>=0})
            var limited=group.states.some(function(state){return ["WAITING","RATE LIMITED"].indexOf(state.result)>=0 && (state.limitActive || (state.result==="RATE LIMITED" && state.limitActive===undefined))})
            group.status=processing ? "Submitting" : limited ? "API Limit" : pending ? "Queued" : group.errors.length ? "ERROR" : "OK"
            return group
        })
    }
    function cooldownLabel(seconds) {return Math.floor(seconds/60)+":"+String(seconds%60).padStart(2,"0")}
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
                text: row.episode.movie ? row.episode.title : row.episode.number + ". " + row.episode.title
                textFormat: Text.PlainText
                enabled: !row.preview && Boolean(row.episode.itemUrl)
                color: row.theme.text; font.pixelSize: 17; font.weight: Font.Bold
                elide: Text.ElideRight; Layout.maximumWidth: row.width-160
                MouseArea {
                    id: episodeLink
                    objectName: "openEpisode_" + row.episode.id
                    anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Link; Accessible.name: "Open " + row.episode.title + " in media server"
                    onClicked: Qt.openUrlExternally(row.episode.itemUrl)
                    Keys.onReturnPressed: Qt.openUrlExternally(row.episode.itemUrl)
                    Keys.onSpacePressed: Qt.openUrlExternally(row.episode.itemUrl)
                }
            }
            AppButton {
                objectName: "openEditor_" + row.episode.id
                text: "▻"; hint: "Open video editor"; iconName: "clapboard"
                implicitWidth: 26; implicitHeight: 24; padding: 4; theme: row.theme
                onClicked: row.editorRequested()
            }
            Item { Layout.fillWidth: true }
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
            model: row.endpointStatuses
            Row {
                id: endpointStatus
                required property var modelData
                objectName: "submissionEndpoint_"+row.episode.id+"_"+modelData.endpoint
                anchors.right: parent.right
                height: 18; spacing: 5
                Text {
                    objectName: "submitStatus_"+row.episode.id+"_"+endpointStatus.modelData.endpoint
                    anchors.verticalCenter: parent.verticalCenter
                    text: endpointStatus.modelData.name+": "+endpointStatus.modelData.status
                    color: endpointStatus.modelData.status==="ERROR" ? "#ff6b6b" : endpointStatus.modelData.status==="API Limit" ? "#ffb833" : row.theme.text
                    font.pixelSize: 12; font.bold: true
                }
                Text {
                    objectName: "submissionCooldown_"+row.episode.id+"_"+endpointStatus.modelData.endpoint
                    visible: endpointStatus.modelData.status==="API Limit" && endpointStatus.modelData.cooldown>0
                    anchors.verticalCenter: parent.verticalCenter
                    text: row.cooldownLabel(endpointStatus.modelData.cooldown)
                    color: "#ffb833"; font.pixelSize: 12; font.bold: true
                }
                AbstractButton {
                    id: errorInfo
                    objectName: "submissionFailure_"+row.episode.id+"_"+endpointStatus.modelData.endpoint
                    visible: endpointStatus.modelData.status==="ERROR"
                    width: 18; height: 18; activeFocusOnTab: true; hoverEnabled: true
                    Accessible.name: endpointStatus.modelData.name+" submission error details"
                    HoverHandler {cursorShape:Qt.PointingHandCursor}
                    background: Rectangle {
                        radius: 9; color: row.theme.hover
                        border.width: errorInfo.activeFocus ? 1 : 0; border.color: row.theme.accent
                        Text {anchors.centerIn:parent;text:"i";font.pixelSize:12;font.bold:true;color:row.theme.text}
                    }
                    onClicked: row.submissionErrorRequested(endpointStatus.modelData.name+" submission error",endpointStatus.modelData.errors.join("\n\n"))
                    AppToolTip {visible:errorInfo.hovered;text:"Error details";theme:row.theme}
                }
            }
        }
    }

}
