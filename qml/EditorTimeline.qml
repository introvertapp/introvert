import QtQuick
import QtQuick.Controls

// Geometry, tick spacing, fills and hit rules ported from SegmentVideoEditor.tsx.
// Only local drafts and the presentation playhead are changed here.
Item {
    id: timeline
    required property var editor
    required property var theme
    objectName: "editorTimeline"
    readonly property real duration: editor.durationSeconds
    readonly property real span: duration > 0 ? duration / editor.zoom : 0
    readonly property real trackLeft: Math.max(10,width*0.012)
    readonly property real trackWidth: Math.max(1,width-trackLeft*2)
    readonly property real trackHeight: Math.max(20,height-34)
    property var dragState: null
    property bool editStarted: false
    property var touchPoints: ({})
    property var touchGesture: null
    function singleTouch(id, point, mode) {
        touchGesture={mode:mode,id:id,x:point.x,y:point.y,start:editor.viewStart,span:span}
    }
    function beginPinch() {
        var ids=Object.keys(touchPoints),a=touchPoints[ids[0]],b=touchPoints[ids[1]]
        if(touchGesture && touchGesture.before) {
            editor.segments=touchGesture.before.segments;editor.textDrafts=touchGesture.before.texts;editor.undoHistory=touchGesture.before.history;editor.draftChanged=editor.hasDraftChanges()
        }
        dragState=null
        var ratio=Math.max(0,Math.min(1,((a.x+b.x)/2-trackLeft)/trackWidth))
        touchGesture={mode:"pinch",distance:Math.max(1,Math.hypot(b.x-a.x,b.y-a.y)),span:span,anchor:editor.viewStart+ratio*span}
    }
    function touchUpdate(points, phase) {
        if(!duration)return
        var next=Object.assign({},touchPoints)
        for(var n=0;n<points.length;n++)next[String(points[n].pointId)]={x:points[n].x,y:points[n].y}
        touchPoints=next
        var ids=Object.keys(next)
        if(phase==="press") {
            if(ids.length>=2){beginPinch();return}
            var id=ids[0],point=next[id],hit=hitAt(point.x,point.y),wasActive=hit && editor.selectedSegment===hit.index
            editor.selectedSegment=hit?hit.index:-1;editor.selectedIndices=hit?[hit.index]:[];editor.draftError=""
            singleTouch(id,point,point.y<24?"scrub":"pending")
            if(hit && (hit.edge || wasActive)) {
                var item=editor.segments[hit.index]
                touchGesture.mode=hit.edge || "move";touchGesture.index=hit.index;touchGesture.segmentStart=item.start;touchGesture.segmentEnd=item.end;touchGesture.time=timeAt(point.x)
                touchGesture.before={segments:editor.segments.slice(),texts:Object.assign({},editor.textDrafts),history:editor.undoHistory.slice()}
            } else if(touchGesture.mode==="scrub")editor.cursorSeconds=timeAt(point.x)
            return
        }
        var g=touchGesture
        if(!g)return
        if(g.mode==="pinch") {
            if(phase!=="release" && ids.length>=2) {
                var a=next[ids[0]],b=next[ids[1]],ratio=Math.max(0,Math.min(1,((a.x+b.x)/2-trackLeft)/trackWidth))
                var nextSpan=g.span*g.distance/Math.max(1,Math.hypot(b.x-a.x,b.y-a.y))
                setView(g.anchor-ratio*nextSpan,nextSpan)
            }
        } else {
            var point=next[g.id]
            if(point) {
                var dx=point.x-g.x,dy=point.y-g.y
                if(g.mode==="pending" && Math.hypot(dx,dy)>=8)g.mode=g.span<duration-.001?"pan":"scrub"
                if(g.mode==="pan")setView(g.start-dx*g.span/trackWidth,g.span)
                else if(g.mode==="scrub" || (phase==="release" && g.mode==="pending"))editor.cursorSeconds=timeAt(point.x)
                else if(phase==="release" && g.before && !g.remembered && g.mode==="move")editor.cursorSeconds=timeAt(point.x)
                else if(g.before && (Math.abs(dx)>1 || Math.abs(dy)>1)) {
                    if(!g.remembered){editor.remember();g.remembered=true}
                    var delta=timeAt(point.x)-g.time
                    if(g.mode==="move") {editor.moveSegment(g.index,g.segmentStart+delta-editor.segments[g.index].start);editor.cursorSeconds=editor.segments[g.index].start}
                    else {editor.resizeSegment(g.index,g.mode,(g.mode==="start"?g.segmentStart:g.segmentEnd)+delta);editor.cursorSeconds=editor.segments[g.index][g.mode]}
                }
            }
        }
        if(phase==="release") {
            for(var n=0;n<points.length;n++)delete next[String(points[n].pointId)]
            touchPoints=next;ids=Object.keys(next)
            if(g.mode==="pinch" && ids.length>=2)beginPinch()
            else if(g.mode==="pinch" && ids.length===1)singleTouch(ids[0],next[ids[0]],"pan")
            else touchGesture=null
        }
    }
    function timeAt(x) { var ratio=Math.max(0,Math.min(1,(x-trackLeft)/trackWidth));return Math.max(0,Math.min(duration,editor.viewStart+ratio*span)) }
    function xAt(time) { return trackLeft+(time-editor.viewStart)/span*trackWidth }
    function setView(start, nextSpan) {
        if(!duration) return
        nextSpan=Math.max(Math.min(2,duration),Math.min(duration,nextSpan))
        editor.zoom=duration/nextSpan
        editor.viewStart=Math.max(0,Math.min(duration-nextSpan,start))
    }
    function wheelAt(x, dx, dy, shift) {
        if(!duration) return
        if(editor.zoom>1 && (Math.abs(dx)>Math.abs(dy) || shift)) {
            var delta=shift && Math.abs(dx)<=Math.abs(dy) ? dy : dx
            setView(editor.viewStart+delta*span/trackWidth,span)
        } else {
            var ratio=Math.max(0,Math.min(1,(x-trackLeft)/trackWidth))
            var anchor=editor.viewStart+ratio*span
            var next=span*Math.max(0.92,Math.min(1.08,1+dy*0.001))
            setView(anchor-ratio*next,next)
        }
    }
    function hitAt(x,y) {
        if(!editor.editable)return null
        if(!span || y<24 || y>height-10) return null
        var order=[]
        if(editor.selectedSegment>=0) order.push(editor.selectedSegment)
        for(var i=0;i<editor.segments.length;i++) if(i!==editor.selectedSegment) order.push(i)
        for(var n=0;n<order.length;n++) {
            var index=order[n], item=editor.segments[index]
            if(item.end-item.start<5 || item.end-item.start>180 || !isFinite(item.start) || !isFinite(item.end)) continue
            var l=xAt(item.start),r=xAt(item.end)
            if(r<trackLeft || l>width-trackLeft) continue
            if(index===editor.selectedSegment) {
                if(l>=trackLeft && l<=width-trackLeft && Math.abs(x-l)<=14) return {index:index,edge:"start"}
                if(r>=trackLeft && r<=width-trackLeft && Math.abs(x-r)<=14) return {index:index,edge:"end"}
            }
            if(x>=Math.max(trackLeft,l) && x<=Math.min(width-trackLeft,r)) return {index:index,edge:""}
        }
        return null
    }
    function redraw() { drawing.requestPaint() }
    Component.onCompleted: redraw()
    onVisibleChanged: if(visible) redraw()
    onDurationChanged: redraw()
    onWidthChanged: redraw()
    onHeightChanged: redraw()
    Connections {
        target: timeline.editor
        function onSegmentsChanged() { timeline.redraw() }
        function onZoomChanged() { timeline.redraw() }
        function onViewStartChanged() { timeline.redraw() }
        function onSelectedSegmentChanged() { timeline.redraw() }
        function onSelectedIndicesChanged() { timeline.redraw() }
    }
    Canvas {
        id: drawing
        objectName: "timelineCanvas"
        anchors.fill: parent; antialiasing: true
        onAvailableChanged: if(available) requestPaint()
        onPaint: {
            var c=getContext("2d");c.reset()
            c.fillStyle="rgba(255,255,255,0.035)";c.fillRect(0,0,width,height)
            var l=timeline.trackLeft,w=timeline.trackWidth,top=24,h=timeline.trackHeight
            c.fillStyle="rgba(255,255,255,0.06)";c.fillRect(l,top,w,h)
            if(!timeline.span) return
            var start=editor.viewStart,end=start+timeline.span,pps=w/timeline.span
            function interval(target, values) { for(var i=0;i<values.length;i++) if(values[i]*pps>=target) return values[i]; return values[values.length-1] }
            function x(time) { return timeline.xAt(time) }
            function rounded(px,py,rw,rh,r) {
                r=Math.max(0,Math.min(r,rw/2,rh/2));c.beginPath();c.moveTo(px+r,py);c.lineTo(px+rw-r,py);c.quadraticCurveTo(px+rw,py,px+rw,py+r);c.lineTo(px+rw,py+rh-r);c.quadraticCurveTo(px+rw,py+rh,px+rw-r,py+rh);c.lineTo(px+r,py+rh);c.quadraticCurveTo(px,py+rh,px,py+rh-r);c.lineTo(px,py+r);c.quadraticCurveTo(px,py,px+r,py);c.closePath()
            }
            c.font=Qt.platform.os === "osx" ? '9px Menlo' : '9px monospace';c.textBaseline="top"
            var labels=[],labelInterval=interval(c.measureText(timeline.duration<3600?"00:00":"00:00:00").width+46,[30,60,120,300,600,900,1800])
            for(var tick=Math.ceil(start/labelInterval)*labelInterval;tick<=end;tick+=labelInterval) {
                var text=editor.headerTime(tick),lx=x(tick)+4,r=lx+c.measureText(text).width+4,last=labels[labels.length-1]
                if(r<=width-l && (!last || lx-last.right>18)) labels.push({left:lx,right:r,text:text})
            }
            function headerTick(px,t,b) {
                var block=null
                for(var i=0;i<labels.length;i++) if(px>=labels[i].left && px<=labels[i].right && b>=4.5 && t<=19.5) { block=labels[i];break }
                c.beginPath()
                if(!block) {c.moveTo(px,t);c.lineTo(px,b)}
                else { if(t<4.5){c.moveTo(px,t);c.lineTo(px,3.5)} if(b>19.5){c.moveTo(px,20.5);c.lineTo(px,b)} }
                c.stroke()
            }
            var frame=1/editor.frameRate
            if(frame*pps>=2.5 && Math.ceil(timeline.span/frame)<=2400) {
                c.strokeStyle="rgba(220,222,185,0.45)";c.lineWidth=1
                for(tick=Math.ceil(start/frame)*frame;tick<=end;tick+=frame) headerTick(x(tick),2,5)
            }
            var seconds=interval(10,[1,2,5,10,15,30]),minutes=interval(70,[60,120,300,600,900,1800])
            c.lineWidth=1;c.strokeStyle="rgba(255,255,255,0.12)"
            for(tick=Math.ceil(start/seconds)*seconds;tick<=end;tick+=seconds) headerTick(x(tick),1,15)
            c.strokeStyle="rgba(255,255,255,0.42)"
            for(tick=Math.ceil(start/minutes)*minutes;tick<=end;tick+=minutes) headerTick(x(tick),1,22)
            c.fillStyle="rgba(255,255,255,0.58)"
            for(var j=0;j<labels.length;j++) c.fillText(labels[j].text,labels[j].left+2,6.5)
            var fills=["rgba(51,209,122,0.18)","rgba(0,209,205,0.18)","rgba(154,167,255,0.18)","rgba(255,184,51,0.18)"]
            var rectangles=[]
            for(j=0;j<editor.segments.length;j++) {
                var item=editor.segments[j]
                if(!isFinite(item.start)||!isFinite(item.end)||item.end-item.start<5||item.end-item.start>180) continue
                var rawL=x(item.start),rawR=x(item.end)
                if(rawR<l || rawL>width-l) continue
                var sl=Math.max(l,rawL),sr=Math.min(width-l,rawR),sw=Math.max(1,sr-sl)
                rectangles.push({index:j,left:sl,right:sr,rawLeft:rawL,rawRight:rawR})
                c.fillStyle=fills[j];rounded(sl,top,sw,h,4);c.fill()
                if(sw>18) {
                    c.save();rounded(sl,top,sw,h,4);c.clip();c.font=Qt.platform.os === "osx" ? '11px "Helvetica Neue"' : '11px sans-serif';c.textBaseline="middle";c.fillStyle="rgba(255,255,255,0.84)";c.shadowColor="rgba(0,0,0,.45)";c.shadowBlur=2
                    c.fillText(item.label+" "+editor.editTime(item.start)+" - "+editor.editTime(item.end),sl+8,top+h/2);c.restore()
                }
            }
            for(j=0;j<rectangles.length;j++) {
                var rect=rectangles[j]
                if(editor.selectedIndices.indexOf(rect.index)<0)continue
                sl=rect.left;sr=rect.right;sw=Math.max(1,sr-sl)
                var z=Math.max(0,Math.min(1,1-timeline.span/timeline.duration)),lw=.95+z*.55
                rounded(sl+lw/2,top+lw/2,Math.max(1,sw-lw),h-lw,4+z)
                c.strokeStyle="rgba(0,0,0,.14)";c.lineWidth=lw+.6;c.stroke()
                rounded(sl+lw/2,top+lw/2,Math.max(1,sw-lw),h-lw,4+z)
                c.strokeStyle="rgba(255,209,42,"+(.58+z*.12)+")";c.lineWidth=lw;c.stroke()
                if(rect.index===editor.selectedSegment) {
                    var hw=3+z*3,hh=Math.min(h-30,Math.max(10,h*(.16+z*.1))),hy=top+(h-hh)/2,shadow=.5+z*.25
                    function handle(px) {
                        c.fillStyle="rgba(0,0,0,.18)";rounded(px-hw/2-shadow,hy-shadow,hw+2*shadow,hh+2*shadow,3+shadow);c.fill()
                        c.fillStyle="#ffd12a";rounded(px-hw/2,hy,hw,hh,3);c.fill()
                        c.fillStyle="rgba(38,33,12,.34)";var gh=Math.max(4,hh-6);rounded(px-.5,hy+(hh-gh)/2,1,gh,.75);c.fill()
                    }
                    if(rect.rawLeft>=l && rect.rawLeft<=width-l)handle(rect.rawLeft)
                    if(rect.rawRight>=l && rect.rawRight<=width-l)handle(rect.rawRight)
                }
            }
        }
    }
    Rectangle { visible: timeline.span>0 && x>=timeline.trackLeft && x<=timeline.width-timeline.trackLeft; x: timeline.span ? timeline.xAt(editor.cursorSeconds) : 0; y: 1; width: 1; height: parent.height-2; color: "#e6ffffff" }
    MouseArea {
        id: pointer
        objectName: "timelinePointer"
        anchors.fill: parent; hoverEnabled: true; enabled: timeline.duration>0
        readonly property var hoverHit: pointerHover.hovered ? timeline.hitAt(pointerHover.point.position.x,pointerHover.point.position.y) : null
        cursorShape: !pointerHover.hovered && !pressed ? Qt.ArrowCursor : timeline.dragState && timeline.dragState.mode==="move" ? Qt.ClosedHandCursor : hoverHit && hoverHit.edge ? Qt.SizeHorCursor : hoverHit && hoverHit.index===editor.selectedSegment ? Qt.OpenHandCursor : Qt.PointingHandCursor
        onPressed: function(mouse) {
            forceActiveFocus();var hit=timeline.hitAt(mouse.x,mouse.y),time=timeline.timeAt(mouse.x)
            timeline.editStarted=false
            if(hit && (mouse.modifiers & (Qt.ControlModifier|Qt.MetaModifier))) {
                var selected=editor.selectedIndices.slice(),at=selected.indexOf(hit.index)
                if(at>=0)selected.splice(at,1);else selected.push(hit.index)
                selected.sort(function(a,b){return a-b});editor.selectedIndices=selected;editor.selectedSegment=selected.indexOf(hit.index)>=0?hit.index:selected.indexOf(editor.selectedSegment)>=0?editor.selectedSegment:selected.length?selected[0]:-1;return
            }
            var active=hit && hit.index===editor.selectedSegment
            editor.selectedSegment=hit?hit.index:-1;editor.selectedIndices=hit?[hit.index]:[]
            editor.cursorSeconds=time
            if(hit && (hit.edge || active)) {
                var item=editor.segments[hit.index]
                timeline.dragState={mode:hit.edge || "move",index:hit.index,start:item.start,end:item.end,time:time,x:mouse.x}
            } else timeline.dragState={mode:"scrub"}
        }
        onPositionChanged: function(mouse) {
            var drag=timeline.dragState
            if(!pressed || !drag)return
            var time=timeline.timeAt(mouse.x)
            if(drag.mode==="scrub") {editor.cursorSeconds=time;return}
            if(!timeline.editStarted && Math.abs(mouse.x-drag.x)<=1)return
            if(!timeline.editStarted){editor.remember();timeline.editStarted=true}
            var delta=time-drag.time
            if(drag.mode==="move") {
                var target=Math.max(0,Math.min(timeline.duration-(drag.end-drag.start),drag.start+delta))
                editor.moveSegment(drag.index,target-editor.segments[drag.index].start);editor.cursorSeconds=target
            } else {
                var edge=(drag.mode==="start"?drag.start:drag.end)+delta
                editor.resizeSegment(drag.index,drag.mode,edge);editor.cursorSeconds=editor.segments[drag.index][drag.mode]
            }
        }
        onReleased: timeline.dragState=null
        onCanceled: timeline.dragState=null
    }
    HoverHandler { id: pointerHover; enabled: timeline.duration>0; cursorShape: pointer.cursorShape }
    function handleWheel(event, horizontal) {
        var pixels=event.pixelDelta.x!==0 || event.pixelDelta.y!==0
        var dx=pixels?event.pixelDelta.x:event.angleDelta.x,dy=pixels?event.pixelDelta.y:event.angleDelta.y
        if(horizontal !== (Math.abs(dx)>Math.abs(dy)))return
        wheelAt(event.x,-dx,-dy,Boolean(event.modifiers & Qt.ShiftModifier))
        event.accepted=true
    }
    // Separate axis handlers accept horizontal-only events. Dominant-axis routing
    // ensures diagonal events apply once even when Qt invokes both handlers.
    WheelHandler { orientation: Qt.Horizontal; acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad; onWheel: function(event) { timeline.handleWheel(event,true) } }
    WheelHandler { orientation: Qt.Vertical; acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad; onWheel: function(event) { timeline.handleWheel(event,false) } }
    PinchHandler {
        id: trackpadPinch
        target: null; acceptedDevices: PointerDevice.TouchPad
        property real initialSpan: 0
        property real anchor: 0
        function updateView() {
            if(!active || !initialSpan)return
            var ratio=Math.max(0,Math.min(1,(centroid.position.x-timeline.trackLeft)/timeline.trackWidth))
            var next=initialSpan/activeScale
            timeline.setView(anchor-ratio*next,next)
        }
        onActiveChanged: if(active) {
            initialSpan=timeline.span
            var ratio=Math.max(0,Math.min(1,(centroid.position.x-timeline.trackLeft)/timeline.trackWidth))
            anchor=editor.viewStart+ratio*initialSpan
        }
        onActiveScaleChanged: updateView()
        onActiveTranslationChanged: updateView()
    }
    // Touchscreen gestures mirror the original pointer state machine. Mouse events
    // stay in MouseArea; macOS trackpad/Magic Mouse scrolling arrives as wheel deltas.
    MultiPointTouchArea {
        anchors.fill: parent; mouseEnabled: false; minimumTouchPoints: 1; maximumTouchPoints: 2
        onPressed: function(points) { timeline.touchUpdate(points,"press") }
        onUpdated: function(points) { timeline.touchUpdate(points,"move") }
        onReleased: function(points) { timeline.touchUpdate(points,"release") }
        onCanceled: { timeline.touchPoints=({});timeline.touchGesture=null;timeline.dragState=null }
    }
}
