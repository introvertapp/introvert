import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia

Dialog {
    id: editor
    required property var theme
    property bool preview: false
    property var backend: null
    readonly property bool editable: preview || Boolean(backend && !backend.busy && backend.editingAllowed)
    property bool receivingPosition: false
    property bool closeAfterSave: false
    property var pendingNavigation: null
    property bool resumeAfterSave: false
    property var previewEpisodes: []
    property var previewSeasonNumbers: [1]
    property int previewSeason: 1
    property string previewEpisodeId: ""
    property var pendingVideoTap: null
    property int videoSkipDirection: 0
    property int videoSkipFrames: 5
    function videoTap(x,y,width) {
        if(!backend || !backend.ready)return
        var side=x<width/2?-1:1,now=Date.now(),previous=pendingVideoTap
        videoTapTimer.stop()
        if(previous && previous.side===side && now-previous.time<=320 && Math.hypot(x-previous.x,y-previous.y)<=96) {
            pendingVideoTap=null;videoSkipDirection=side;videoSkipFrames=[5,10,20][frameStepIndex]
            cursorSeconds=Math.max(0,Math.min(durationSeconds,cursorSeconds+side*videoSkipFrames/frameRate))
            skipOverlay.opacity=1;skipFade.restart();return
        }
        if(previous)backend.togglePlayback()
        pendingVideoTap={x:x,y:y,side:side,time:now};videoTapTimer.restart()
    }
    Timer { id:videoTapTimer; interval:320; onTriggered: {editor.pendingVideoTap=null;if(editor.backend)editor.backend.togglePlayback()} }
    SequentialAnimation {
        id:skipFade
        ParallelAnimation {
            NumberAnimation { target:skipOverlay; property:"opacity"; from:0; to:1; duration:136; easing.type:Easing.OutCubic }
            NumberAnimation { target:skipOverlay; property:"scale"; from:.88; to:1; duration:136; easing.type:Easing.OutCubic }
        }
        PauseAnimation { duration:391 }
        ParallelAnimation {
            NumberAnimation { target:skipOverlay; property:"opacity"; to:0; duration:323; easing.type:Easing.OutCubic }
            NumberAnimation { target:skipOverlay; property:"scale"; to:1.04; duration:323; easing.type:Easing.OutCubic }
        }
    }
    property string showTitle: "Video segment editor"
    property string episodeTitle: "No episode loaded"
    property string timestampFormat: "both"
    property real durationSeconds: preview ? 2640 : 0
    property real frameRate: 25
    property bool drawerOpen: true
    property bool draftChanged: false
    property real zoom: 1
    property real viewStart: 0
    property bool fieldEditing: false
    property var selectedIndices: []
    property var textDrafts: ({})
    property string draftError: ""
    property int frameStepIndex: 0
    property int playbackSpeedIndex: 0
    property real cursorSeconds: 0
    property var undoHistory: []
    property int selectedSegment: -1
    readonly property var segmentTemplates: [
        {label:"Recap",start:0,end:28,color:"#33d17a"},
        {label:"Intro",start:42,end:89,color:"#00D1CD"},
        {label:"Credits",start:2530,end:2585,color:"#9aa7ff"},
        {label:"Preview",start:2590,end:2630,color:"#ffb833"}
    ]
    property var initialSegments: preview ? segmentTemplates.slice() : []
    property var segments: initialSegments.slice()
    function loadEpisode(record) {
        if(!preview)return
        previewEpisodeId=record.id || ""
        initialSegments=segmentTemplates.map(function(item) {
            var ends=(record[item.label.toLowerCase()] || "").split(/\s*[–→]\s*/)
            function seconds(value) { var parts=value.split(":"),total=0;for(var n=0;n<parts.length;n++)total=total*60+Number(parts[n]);return total }
            return Object.assign({},item,{start:ends.length===2?seconds(ends[0]):NaN,end:ends.length===2?seconds(ends[1]):NaN})
        })
        resetDrafts();undoHistory=[];cursorSeconds=0
    }
    function loadBackend() {
        if(!backend || preview)return
        videoTapTimer.stop();pendingVideoTap=null;skipFade.stop();skipOverlay.opacity=0
        var values=backend.segmentValues
        initialSegments=segmentTemplates.map(function(item) {
            var range=values[item.label.toLowerCase()]
            return Object.assign({},item,{start:range?range[0]:NaN,end:range?range[1]:NaN})
        })
        resetDrafts(true);undoHistory=[];episodeTitle=backend.episodeTitle
    }
    function saveTimeline(closeAfter) {
        if(!editable || !backend || preview)return
        var rows=[]
        for(var i=0;i<segments.length;i++) {
            var start=draftText(i,"start"),end=draftText(i,"end")
            if((start.length || end.length) && (!isFinite(segments[i].start) || !isFinite(segments[i].end))) {draftError="Segment timestamps are invalid";return}
            rows.push({start:start.length?segments[i].start:null,end:end.length?segments[i].end:null})
        }
        closeAfterSave=Boolean(closeAfter);backend.save(rows)
    }
    function requestNavigation(action) {
        if(backend && backend.busy)return
        if(draftChanged) { pendingNavigation=action;unsaved.open() }
        else action()
    }
    function requestEpisode(record) {
        if((backend && !preview && backend.episodeId===record.id) || (preview && previewEpisodeId===record.id))return
        requestNavigation(function(){
            if(backend && !preview)backend.openEpisode(record.id)
            else { episodeTitle="S"+previewSeason+"・E"+record.number+" - "+record.title;loadEpisode(record) }
        })
    }
    function navigateEpisode(direction) { if(backend && !backend.busy)requestNavigation(function(){backend.navigate(direction)}) }
    function continueNavigation() {
        var action=pendingNavigation
        pendingNavigation=null;draftChanged=false;unsaved.close()
        if(action)action()
    }
    onCursorSecondsChanged: { if(backend && !preview && !receivingPosition)backend.seek(cursorSeconds) }
    onDrawerOpenChanged: { if(timelineWorkspace)timelineWorkspace.setExpanded(drawerOpen); if(backend && !preview && backend.preferences.drawer!==drawerOpen)backend.setPreference("drawer",drawerOpen?1:0) }
    onFrameStepIndexChanged: { if(backend && !preview && backend.preferences.frameStep!==frameStepIndex)backend.setPreference("frameStep",frameStepIndex) }
    onPlaybackSpeedIndexChanged: { if(backend && !preview && backend.preferences.speed!==playbackSpeedIndex)backend.setPreference("speed",playbackSpeedIndex) }
    onOpened: {
        if(backend && !preview) { drawerOpen=backend.preferences.drawer;frameStepIndex=backend.preferences.frameStep;playbackSpeedIndex=backend.preferences.speed }
    }
    onClosed: { videoTapTimer.stop();pendingVideoTap=null;skipFade.stop();skipOverlay.opacity=0;if(backend && !preview)backend.unload();closeAfterSave=false;pendingNavigation=null;resumeAfterSave=false }
    Connections {
        target: editor.backend || null
        function onLoaded() { editor.loadBackend() }
        function onSaved() { if(editor.resumeAfterSave) {editor.resumeAfterSave=false;editor.continueNavigation()} else if(editor.closeAfterSave) { unsaved.close();editor.close() } }
        function onChanged() {
            if(editor.preview || !editor.backend)return
            editor.durationSeconds=editor.backend.duration;editor.frameRate=editor.backend.frameRate
            editor.receivingPosition=true;editor.cursorSeconds=editor.backend.position;editor.receivingPosition=false
        }
    }
    objectName: "videoEditorDialog"
    parent: Overlay.overlay; anchors.centerIn: parent
    width: parent ? parent.width-36 : 1100
    height: parent ? parent.height-36 : 740
    Overlay.modal: Rectangle { color: "#b8000000" }
    modal: true; focus: true; padding: 14; leftPadding: 15; rightPadding: 15; topPadding: 11; bottomPadding: 15; closePolicy: Popup.NoAutoClose
    background: Rectangle { color: editor.theme.background; border.color: editor.theme.border; radius: 8 }
    function requestClose() { if (help.visible) help.close(); else if (draftChanged) { pendingNavigation=null;unsaved.open() } else close() }
    function hasDraftChanges() {
        var baseline=initialSegments
        if(segments.length!==baseline.length)return true
        for(var i=0;i<baseline.length;i++) {
            if(!Object.is(segments[i].start,baseline[i].start) || !Object.is(segments[i].end,baseline[i].end))return true
            for(var n=0;n<2;n++){var edge=n?"end":"start",key=i+"_"+edge;if(Object.prototype.hasOwnProperty.call(textDrafts,key) && textDrafts[key]!==editTime(baseline[i][edge]))return true}
        }
        return false
    }
    function remember() {
        if(!editable)return
        var state={segments:segments.slice(),texts:Object.assign({},textDrafts),selected:selectedIndices.slice(),active:selectedSegment,zoom:zoom,viewStart:viewStart}
        var last=undoHistory[undoHistory.length-1]
        if(!last || JSON.stringify(last)!==JSON.stringify(state))undoHistory=undoHistory.slice(-49).concat([state])
    }
    function clearTextFor(index) {
        var texts=Object.assign({},textDrafts);delete texts[index+"_start"];delete texts[index+"_end"];textDrafts=texts
    }
    function draftText(index,edge) {
        var key=index+"_"+edge
        if(Object.prototype.hasOwnProperty.call(textDrafts,key))return textDrafts[key]
        var item=segments[index]
        return item && isFinite(item[edge]) ? editTime(item[edge]) : ""
    }
    function segmentValid(index) {
        var item=segments[index]
        return Boolean(item && isFinite(item.start) && isFinite(item.end) && item.end-item.start>=5 && item.end-item.start<=180)
    }
    function moveSegment(index, delta) {
        if(!editable)return
        var values = segments.slice(); var item = values[index]; var duration = item.end-item.start
        var start = Math.min(durationSeconds-duration, Math.max(0,item.start+delta))
        values[index] = {label:item.label,start:start,end:start+duration,color:item.color}
        clearTextFor(index); segments = values; draftChanged = hasDraftChanges()
    }
    function undo() {
        if(!editable || !undoHistory.length)return
        var history=undoHistory.slice(),state=history.pop()
        timeline.dragState=null;draftError="";segments=state.segments;textDrafts=state.texts;selectedIndices=state.selected;selectedSegment=state.active;zoom=state.zoom;viewStart=state.viewStart;undoHistory=history;draftChanged=hasDraftChanges()
    }
    function resizeSegment(index, edge, time) {
        if(!editable)return
        var values=segments.slice(); var item=values[index]
        values[index]={label:item.label,start:edge==="start"?Math.max(0,item.end-180,Math.min(time,item.end-5)):item.start,end:edge==="end"?Math.min(durationSeconds,item.start+180,Math.max(time,item.start+5)):item.end,color:item.color}
        clearTextFor(index);segments=values; draftChanged=hasDraftChanges()
    }
    function editTime(seconds) {
        var rounded=Math.floor(Math.round(seconds*1000)/100)/10
        if(timestampFormat === "seconds")return rounded.toFixed(1)
        return String(Math.floor(rounded/60)).padStart(2,"0")+":"+(rounded%60).toFixed(1).padStart(4,"0")
    }
    function transportMarkup(seconds) {
        var tenths=Math.max(0,Math.floor(seconds*10)),total=Math.floor(tenths/10)
        var text=String(Math.floor(total/3600)).padStart(2,"0")+":"+String(Math.floor(total/60)%60).padStart(2,"0")+":"+String(total%60).padStart(2,"0")+":"+(tenths%10)
        var split=text.search(/[1-9]/);if(split<0)split=text.length
        return '<font color="#49494e">'+text.slice(0,split)+'</font><font color="#8d8d92">'+text.slice(split)+'</font>'
    }
    function resetDrafts(fromBackend) {
        if(!editable && !fromBackend)return
        remember();timeline.dragState=null;timeline.touchGesture=null;timeline.touchPoints=({});timeline.editStarted=false;draftError="";segments=initialSegments.slice();textDrafts=({});selectedIndices=[];selectedSegment=-1;zoom=1;viewStart=0;draftChanged=false
    }
    function headerTime(seconds) {
        var minutes=Math.floor(seconds/60),ss=String(Math.floor(seconds%60)).padStart(2,"0")
        return minutes>=60 ? String(Math.floor(minutes/60)).padStart(2,"0")+":"+String(minutes%60).padStart(2,"0")+":"+ss : String(minutes).padStart(2,"0")+":"+ss
    }
    function updateTableDraft(index,edge,text) {
        if(!editable)return
        draftError=""
        var parts=text.trim().split(":"),time=0
        if(parts.length>3 || !text.trim().length)time=NaN
        for(var n=0;n<parts.length;n++) {
            if(!parts[n].length || !isFinite(Number(parts[n])) || Number(parts[n])<0){time=NaN;break}
            time=time*60+Number(parts[n])
        }
        if(parts.length===1 && !/^\d+(\.\d+)?$/.test(text.trim()))time=NaN
        var texts=Object.assign({},textDrafts);texts[index+"_"+edge]=text;textDrafts=texts
        var values=segments.slice(),item=Object.assign({},values[index]);item[edge]=Math.round(time*1000)/1000;values[index]=item;segments=values;draftChanged=hasDraftChanges()
    }
    function insertDraft(index) {
        if(!editable)return
        var existing=segments[index]
        if(segmentValid(index)) {selectedSegment=index;selectedIndices=[index];cursorSeconds=index>1?existing.end:existing.start;return}
        var start=cursorSeconds
        draftError=""
        if((index<2 && start>durationSeconds/2)||(index>1 && start<durationSeconds/2)) {draftError=initialSegments[index].label+" segment can only be inserted in the "+(index<2?"first":"second")+" half of the video";return}
        if(durationSeconds-start<5){draftError="At least 5 seconds are required after the playhead";return}
        var values=segments.slice();remember();clearTextFor(index);values[index]=Object.assign({},initialSegments[index],{start:start,end:Math.min(durationSeconds,start+(index>1?30:15))});segments=values;selectedSegment=index;selectedIndices=[index];draftChanged=true
    }
    function moveNearest(index) {
        var item=segments[index]
        if(!editable)return
        if(!item || !isFinite(item.start) || !isFinite(item.end)){draftError=initialSegments[index].label+" segment must exist before its position can be changed";return}
        var edge=Math.abs(cursorSeconds-item.start)<=Math.abs(cursorSeconds-item.end)?"start":"end"
        remember();resizeSegment(index,edge,cursorSeconds);selectedSegment=index;selectedIndices=[index]
    }
    function selectAllDrafts() {
        var selected=[]
        for(var n=0;n<segments.length;n++) if(isFinite(segments[n].start) && isFinite(segments[n].end) && segments[n].end-segments[n].start>=5 && segments[n].end-segments[n].start<=180)selected.push(n)
        selectedIndices=selected;selectedSegment=selected.length?selected[0]:-1
    }
    function deleteDrafts() {
        if(!editable || !selectedIndices.length)return
        remember();var values=segments.slice()
        for(var n=0;n<selectedIndices.length;n++){var index=selectedIndices[n];clearTextFor(index);values[index]=Object.assign({},values[index],{start:NaN,end:NaN})}
        segments=values;selectedSegment=-1;selectedIndices=[];draftChanged=true
    }
    function formatTime(seconds) { return String(Math.floor(seconds/60)).padStart(2,"0") + ":" + String(Math.floor(seconds%60)).padStart(2,"0") }
    Shortcut { sequence: "Escape"; enabled: editor.visible; onActivated: editor.requestClose() }
    Shortcut { sequences: [StandardKey.Undo]; enabled: editor.visible && editor.editable && !editor.fieldEditing; onActivated: editor.undo() }
    Shortcut { sequence: "H"; enabled: editor.visible && !editor.fieldEditing; onActivated: help.visible ? help.close() : help.open() }
    Shortcut { sequence: "S"; enabled: editor.visible && !editor.fieldEditing; onActivated: editor.drawerOpen = !editor.drawerOpen }
    Shortcut { sequence: "Shift+Z"; enabled: editor.visible && !editor.fieldEditing; onActivated: { editor.zoom=1;editor.viewStart=0 } }
    Shortcut { sequence: "Ctrl+A"; enabled: editor.visible && !editor.fieldEditing; onActivated: editor.selectAllDrafts() }
    Shortcut { sequence: "Meta+A"; enabled: editor.visible && !editor.fieldEditing; onActivated: editor.selectAllDrafts() }
    Shortcut { sequences: ["Delete","Backspace"]; enabled: editor.visible && !editor.fieldEditing; onActivated: editor.deleteDrafts() }
    Shortcut { sequence: "R"; enabled: editor.visible && !editor.fieldEditing; onActivated: editor.insertDraft(0) }
    Shortcut { sequence: "Shift+R"; enabled: editor.visible && !editor.fieldEditing; onActivated: editor.moveNearest(0) }
    Shortcut { sequence: "I"; enabled: editor.visible && !editor.fieldEditing; onActivated: editor.insertDraft(1) }
    Shortcut { sequence: "Shift+I"; enabled: editor.visible && !editor.fieldEditing; onActivated: editor.moveNearest(1) }
    Shortcut { sequence: "C"; enabled: editor.visible && !editor.fieldEditing; onActivated: editor.insertDraft(2) }
    Shortcut { sequence: "Shift+C"; enabled: editor.visible && !editor.fieldEditing; onActivated: editor.moveNearest(2) }
    Shortcut { sequence: "P"; enabled: editor.visible && !editor.fieldEditing; onActivated: editor.insertDraft(3) }
    Shortcut { sequence: "Shift+P"; enabled: editor.visible && !editor.fieldEditing; onActivated: editor.moveNearest(3) }
    Shortcut { sequence: "Home"; enabled: editor.visible && (editor.preview || Boolean(editor.backend && editor.backend.ready)) && !editor.fieldEditing; onActivated: editor.cursorSeconds=0 }
    Shortcut { sequence: "End"; enabled: editor.visible && (editor.preview || Boolean(editor.backend && editor.backend.ready)) && !editor.fieldEditing; onActivated: editor.cursorSeconds=editor.durationSeconds }
    Shortcut { sequence: "Left"; enabled: editor.visible && (editor.preview || Boolean(editor.backend && editor.backend.ready)) && !editor.fieldEditing; onActivated: editor.cursorSeconds=Math.max(0,editor.cursorSeconds-[5,10,20][editor.frameStepIndex]/editor.frameRate) }
    Shortcut { sequence: "Right"; enabled: editor.visible && (editor.preview || Boolean(editor.backend && editor.backend.ready)) && !editor.fieldEditing; onActivated: editor.cursorSeconds=Math.min(editor.durationSeconds,editor.cursorSeconds+[5,10,20][editor.frameStepIndex]/editor.frameRate) }
    Shortcut { sequence: "Shift+Left"; enabled: editor.visible && (editor.preview || Boolean(editor.backend && editor.backend.ready)) && !editor.fieldEditing; onActivated: editor.cursorSeconds=Math.max(0,editor.cursorSeconds-1/editor.frameRate) }
    Shortcut { sequence: "Shift+Right"; enabled: editor.visible && (editor.preview || Boolean(editor.backend && editor.backend.ready)) && !editor.fieldEditing; onActivated: editor.cursorSeconds=Math.min(editor.durationSeconds,editor.cursorSeconds+1/editor.frameRate) }
    Shortcut { sequence: "Ctrl+R"; enabled: editor.visible && !editor.fieldEditing; onActivated: editor.resetDrafts() }
    Shortcut { sequence: "Meta+R"; enabled: editor.visible && !editor.fieldEditing; onActivated: editor.resetDrafts() }
    Shortcut { sequences: ["Space","K"]; enabled: editor.visible && !editor.fieldEditing; onActivated: { if(editor.backend)editor.backend.togglePlayback() } }
    Shortcut { sequence: "J"; enabled: editor.visible && !editor.fieldEditing; onActivated: editor.navigateEpisode(-1) }
    Shortcut { sequence: "L"; enabled: editor.visible && !editor.fieldEditing; onActivated: editor.navigateEpisode(1) }
    Shortcut { sequences: ["Ctrl+S","Meta+S"]; enabled: editor.visible && !editor.fieldEditing; onActivated: editor.saveTimeline(false) }
    contentItem: ColumnLayout {
        spacing: 8
        RowLayout {
            Layout.preferredHeight: 26; Layout.bottomMargin: 2
            Item { width: 26; height: 20 }
            Text { Layout.fillWidth: true; text: editor.episodeTitle; horizontalAlignment: Text.AlignHCenter; elide: Text.ElideRight; color: editor.theme.text; font.bold: true; font.pixelSize: 16 }

            AppButton { objectName: "editorCloseControl"; glyphOnly: true; fontSize: 22; text: "×"; implicitWidth: 26; implicitHeight: 26; hint: "Close video editor"; theme: editor.theme; onClicked: editor.requestClose() }
        }
        Rectangle {
            objectName: "editorVideoSurface"
            Layout.fillWidth: true; Layout.fillHeight: true; Layout.minimumHeight: editor.height < 650 ? 60 : 140
            color: editor.theme.card; radius: 6; clip: true
            CollapsibleSplitView {
                id: videoWorkspace
                objectName: "editorVideoWorkspace"
                anchors.fill: parent; anchors.margins: 1
                theme: editor.theme; panel: episodeDrawer
                collapsedWidth: 0; expandedWidth: 280; retainDraggedWidth: true
                handleName: "editorEpisodesDivider"
                separatorName: "editorEpisodesSeparator"
                gripName: "editorEpisodesGrip"
                EditorEpisodeDrawer {
                    id: episodeDrawer
                    objectName: "editorEpisodeDrawer"
                    editor: editor; theme: editor.theme; reveal: videoWorkspace.panelReveal
                    SplitView.minimumWidth: 0; SplitView.maximumWidth: Math.min(460,videoWorkspace.width-210)
                    SplitView.preferredWidth: 280
                }
                Rectangle {
                    objectName: "videoPreviewRegion"
                    SplitView.fillWidth: true; SplitView.minimumWidth: 200
                    color: "black"
                    VideoOutput {
                        id: videoOutput; objectName: "editorVideoOutput"; anchors.fill: parent
                        fillMode: VideoOutput.PreserveAspectFit
                    }
                    Connections { target: editor.backend || null; function onChanged() { if(editor.backend)editor.backend.attachVideoSink(videoOutput.videoSink) } }
                    MouseArea {
                        objectName:"editorVideoMouse"; anchors.fill:parent; cursorShape:Qt.PointingHandCursor
                        enabled:Boolean(editor.backend && editor.backend.ready); acceptedButtons:Qt.LeftButton
                        onClicked:editor.backend.togglePlayback()
                    }
                    MultiPointTouchArea {
                        anchors.fill:parent; mouseEnabled:false; minimumTouchPoints:1; maximumTouchPoints:1
                        property var startPoint:null
                        onPressed:function(points) {startPoint=points.length?{x:points[0].x,y:points[0].y}:null}
                        onReleased:function(points) {
                            if(startPoint && points.length && Math.hypot(points[0].x-startPoint.x,points[0].y-startPoint.y)<=14)editor.videoTap(points[0].x,points[0].y,width)
                            startPoint=null
                        }
                        onCanceled:startPoint=null
                    }
                    Row {
                        id:skipOverlay; opacity:0; spacing:5; anchors.verticalCenter:parent.verticalCenter
                        x:editor.videoSkipDirection<0?parent.width*.13:parent.width*.87-width
                        AppImage { visible:editor.videoSkipDirection<0; source:"icons/chevron-left.svg"; width:18;height:18 }
                        Text { text:(editor.videoSkipDirection<0?"−":"+")+" "+editor.videoSkipFrames;color:"#f5ffffff";font.pixelSize:14;font.weight:Font.DemiBold }
                        AppImage { visible:editor.videoSkipDirection>0; source:"icons/chevron-right.svg";width:18;height:18 }
                    }
                    Skeleton { anchors.fill: parent; theme: editor.theme; visible: Boolean(editor.backend && editor.backend.busy) }
                    Column { visible: (!editor.backend || !editor.backend.ready) && !(editor.backend && editor.backend.busy); anchors.centerIn: parent; spacing: 10; width: parent.width-40
                        Text { width: parent.width; text: editor.backend && editor.backend.busy ? "Loading video…" : "Video preview unavailable"; horizontalAlignment: Text.AlignHCenter; color: "#c5c5ca"; font.pixelSize: 14 }
                        Text { width: parent.width; text: editor.backend ? editor.backend.message : "No media is loaded."; horizontalAlignment: Text.AlignHCenter; color: "#777780"; font.pixelSize: 13 }
                    }
                }
            }
            RoundedFrame { anchors.fill: parent; radius: 6; outsideColor: editor.theme.background; borderColor: editor.theme.border }
        }
        Rectangle {
            Layout.fillWidth: true; implicitHeight: 85
            color: "#121217"
            border.color: editor.theme.border; radius: 6
            ColumnLayout {
                anchors.fill: parent; spacing: 0
                RowLayout {
                    Layout.alignment: Qt.AlignHCenter; Layout.preferredHeight: 38; spacing: 14
                    AppButton { iconName: "chevron-left"; glyphOnly: true; hint: "Previous episode (J)"; implicitWidth: 24; implicitHeight: 26; enabled: Boolean(editor.backend && editor.backend.hasPrevious && !editor.backend.busy); theme: editor.theme; onClicked: editor.navigateEpisode(-1) }
                    Text { text: editor.episodeTitle; color: editor.theme.muted; font.pixelSize: 15; font.bold: true; elide: Text.ElideRight; Layout.maximumWidth: editor.width*0.65 }
                    Text { text: editor.headerTime(editor.durationSeconds); color: editor.theme.muted; font.pixelSize: 15 }
                    AppButton { iconName: "chevron-right"; glyphOnly: true; hint: "Next episode (L)"; implicitWidth: 24; implicitHeight: 26; enabled: Boolean(editor.backend && editor.backend.hasNext && !editor.backend.busy); theme: editor.theme; onClicked: editor.navigateEpisode(1) }
                }
                Rectangle { Layout.fillWidth: true; height: 1; color: editor.theme.border }
                Item {
                    Layout.fillWidth: true; Layout.fillHeight: true
                    RowLayout {
                        anchors.left: parent.left; anchors.leftMargin: 10; anchors.verticalCenter: parent.verticalCenter; spacing: 6
                        AppButton { objectName: "editorHelpControl"; iconOpacity: hovered ? 1 : .55; glyphOnly: true; iconName: "keyboard"; hint: "Keyboard shortcuts (H)"; implicitWidth: 26; implicitHeight: 26; theme: editor.theme; onClicked: help.open() }
                        AppButton { iconName: "play"; objectName: "editorAutoPlayControl"; iconOpacity: hovered || selected ? 1 : .4; glyphOnly: true; hint: "Video auto-play "+(editor.backend && editor.backend.preferences.autoPlay?"on":"off"); selected: Boolean(editor.backend && editor.backend.preferences.autoPlay); implicitWidth: 26; implicitHeight: 26; enabled: Boolean(editor.backend); theme: editor.theme; onClicked: editor.backend.setPreference("autoPlay",editor.backend.preferences.autoPlay?0:1) }
                        AppButton { objectName: "editorFrameStepControl"; subtle: true; text: "⏴"+[5,10,20][editor.frameStepIndex]+"⏵"; implicitWidth: 44; implicitHeight: 26; fontSize: 11; hint: "Arrow keys move "+[5,10,20][editor.frameStepIndex]+" frames"; theme: editor.theme; onClicked: editor.frameStepIndex=(editor.frameStepIndex+1)%3 }
                        AppButton { objectName: "editorSpeedControl"; subtle: true; text: "⏵ "+[1,-2,-3][editor.playbackSpeedIndex]+"x"; implicitWidth: 44; implicitHeight: 26; fontSize: 11; hint: "Playback speed "+[1,-2,-3][editor.playbackSpeedIndex]+"x (click to cycle)"; theme: editor.theme; onClicked: editor.playbackSpeedIndex=(editor.playbackSpeedIndex+1)%3 }
                        AppButton { objectName: "editorMuteControl"; iconOpacity: hovered ? 1 : .55; glyphOnly: true; iconSize: 17; iconName: editor.backend && editor.backend.muted ? "volume-muted" : "volume"; hint: editor.backend && editor.backend.muted ? "Unmute audio" : "Mute audio"; implicitWidth: 26; implicitHeight: 26; enabled: Boolean(editor.backend); theme: editor.theme; onClicked: editor.backend.toggleMuted() }
                    }
                    RowLayout {
                        anchors.centerIn: parent; spacing: 10
                        AppButton { objectName: "editorPlayControl"; iconName: editor.backend && editor.backend.playing ? "pause" : "play"; glyphOnly: true; hint: editor.backend && editor.backend.playing ? "Pause video (Space / K)" : "Play video (Space / K)"; implicitWidth: 24; implicitHeight: 26; enabled: Boolean(editor.backend && editor.backend.ready); theme: editor.theme; onClicked: editor.backend.togglePlayback() }
                        Text { objectName: "editorTransportTime"; Layout.preferredWidth: 156; font.features: ({"tnum":1}); text: editor.transportMarkup(editor.cursorSeconds); textFormat: Text.RichText; color: editor.theme.muted; font.pixelSize: 26; font.weight: Font.Normal }
                        Row { spacing: 4; Repeater { model: 2; Column { required property int index; property real level: editor.backend ? editor.backend.levels[index] : 0; spacing: 1; Repeater { model: 7; Rectangle { required property int index; width: 7; height: 3; color: parent.level >= (7-index)/7 ? (index<1?"#d8655f":index<3?"#d8bd4b":"#4cb878") : "#1affffff" } } } } }
                    }
                    RowLayout {
                        anchors.right: parent.right; anchors.rightMargin: 10; anchors.verticalCenter: parent.verticalCenter; spacing: 8
                        AppButton { objectName: "editorResetControl"; enabled: editor.editable; iconOpacity: hovered ? 1 : .8; glyphOnly: true; iconSize: 16; iconName: "reset"; hint: "Reset timeline drafts and zoom (⌘/Ctrl+R)"; implicitWidth: 26; implicitHeight: 26; theme: editor.theme; onClicked: editor.resetDrafts() }
                        AppButton { objectName: "editorSaveControl"; iconOpacity: hovered ? 1 : .8; glyphOnly: true; iconSize: 16; iconName: "save"; hint: "Save timeline (⌘/Ctrl+S)"; implicitWidth: 26; implicitHeight: 26; enabled: Boolean(editor.editable && editor.backend && editor.draftChanged); theme: editor.theme; onClicked: editor.saveTimeline(false) }
                    }
                }
            }
        }
        Rectangle {
            objectName: "editorTimelineSurface"
            Layout.fillWidth: true; Layout.preferredHeight: 126; Layout.maximumHeight: 126
            color: editor.theme.card; radius: 6; clip: true
            CollapsibleSplitView {
                id: timelineWorkspace
                objectName: "timelineWorkspace"
                anchors.fill: parent; anchors.margins: 1
                theme: editor.theme; panel: segmentDrawer
                collapsedWidth: 0; expandedWidth: 236
                handleName: "editorDrawerControl"
                separatorName: "editorDrawerSeparator"
                gripName: "editorDrawerGrip"
                onPanelStateRequested: function(expanded) { editor.drawerOpen=expanded }
                enabled: !editor.backend || !editor.backend.busy
                EditorDrawer {
                    id: segmentDrawer
                    objectName: "editorSegmentDrawer"
                    editor: editor; theme: editor.theme; reveal: timelineWorkspace.panelReveal
                    SplitView.minimumWidth: 0; SplitView.maximumWidth: 236
                    SplitView.preferredWidth: editor.drawerOpen ? 236 : 0
                }
                EditorTimeline {
                    id: timeline; editor: editor; theme: editor.theme
                    SplitView.fillWidth: true; SplitView.minimumWidth: 120
                    Slider { objectName: "timelineZoomControl"; visible: false; from: 1; to: 1320; value: editor.zoom; onMoved: editor.zoom=value }
                }
            }
            RoundedFrame {
                objectName: "editorTimelineFrame"
                anchors.fill: parent; radius: 6
                outsideColor: editor.theme.background; borderColor: editor.theme.border
            }
        }


        Text { visible: editor.draftError !== "" || Boolean(editor.backend && editor.backend.message); Layout.fillWidth: true; wrapMode: Text.WordWrap; text: editor.draftError || (editor.backend ? editor.backend.message : ""); color: editor.theme.danger; font.pixelSize: 13 }

    }
    ShortcutMap { id:help; objectName:"editorHelpDialog"; theme:editor.theme }
    SurfaceDialog {
        theme: editor.theme; showCloseButton: false
        id:unsaved; objectName:"editorUnsavedDialog"; parent:Overlay.overlay; anchors.centerIn:parent; width:Math.min(500,parent.width-40); title:"Unsaved changes"; modal:true
        onClosed: {editor.pendingNavigation=null;editor.resumeAfterSave=false}
        ColumnLayout { width:parent.width; spacing:15; Text { Layout.fillWidth:true; text:"This timeline has unsaved changes."; wrapMode:Text.WordWrap; color:editor.theme.muted } RowLayout { AppButton { objectName:"editorSaveCloseControl"; text:editor.pendingNavigation ? "Save and continue" : "Save and close"; enabled:Boolean(editor.editable && editor.backend); theme:editor.theme; onClicked: {editor.resumeAfterSave=Boolean(editor.pendingNavigation);editor.saveTimeline(!editor.resumeAfterSave)} } AppButton { objectName:"discardEditorDraft"; text:editor.pendingNavigation ? "Discard and continue" : "Discard and close"; theme:editor.theme; onClicked: { if(editor.pendingNavigation)editor.continueNavigation();else {editor.draftChanged=false;unsaved.close();editor.close()} } } AppButton { objectName:"editorUnsavedCancelControl"; text:"Cancel"; theme:editor.theme; onClicked:unsaved.close() } } }
    }
}
