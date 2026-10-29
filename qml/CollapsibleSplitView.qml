import QtQuick
import QtQuick.Controls

SplitView {
    id: split
    required property var theme
    required property Item panel
    property real collapsedWidth: 80
    property real expandedWidth: 260
    property bool retainDraggedWidth: false
    property real restoredWidth: expandedWidth
    property string handleName: ""
    property string separatorName: ""
    property string gripName: ""
    readonly property real panelReveal: Math.max(0,Math.min(1,(panel.width-collapsedWidth-20)/(expandedWidth-collapsedWidth-60)))
    signal panelStateRequested(bool expanded)

    function setExpanded(expanded) {
        var targetWidth=expanded ? (retainDraggedWidth ? restoredWidth : expandedWidth) : collapsedWidth
        if(widthAnimation.running && widthAnimation.to===targetWidth)return
        widthAnimation.stop()
        widthAnimation.from=panel.width
        widthAnimation.to=targetWidth
        widthAnimation.start()
    }
    function togglePanelWidth() {
        var expand=widthAnimation.running ? widthAnimation.to===collapsedWidth : retainDraggedWidth ? panel.width<=collapsedWidth+1 : panel.width<(collapsedWidth+expandedWidth)/2
        setExpanded(expand)
        panelStateRequested(expand)
    }
    NumberAnimation {
        id: widthAnimation
        objectName: "panelWidthAnimation"
        target: split.panel.SplitView; property: "preferredWidth"
        duration: 160; easing.type: Easing.OutCubic
    }
    onResizingChanged: {
        if(resizing) widthAnimation.stop()
        else {
            if(retainDraggedWidth && panel.width>collapsedWidth+1)restoredWidth=panel.width
            panelStateRequested(panel.width>=(collapsedWidth+expandedWidth)/2)
        }
    }
    orientation: Qt.Horizontal
    handle: Item {
        id: dividerHandle
        objectName: split.handleName
        implicitWidth: 10
        property real pressWidth: 0
        property bool secondClick: false
        Timer { id: clickInterval; interval: Qt.styleHints.mouseDoubleClickInterval }
        // SplitView retains sole control of dragging; observe its press state.
        SplitHandle.onPressedChanged: {
            if(SplitHandle.pressed) {
                pressWidth=split.panel.width
                secondClick=clickInterval.running
                clickInterval.stop()
            } else if(Math.abs(split.panel.width-pressWidth)>1) {
                secondClick=false
            } else if(secondClick) {
                secondClick=false
                Qt.callLater(split.togglePanelWidth)
            } else clickInterval.restart()
        }
        HoverHandler { cursorShape: Qt.SplitHCursor }
        Rectangle { objectName: split.separatorName; anchors.centerIn: parent; width: 1; height: parent.height-2; color: split.theme.border }
        Rectangle { objectName: split.gripName; anchors.centerIn: parent; width: 3; height: 22; radius: 1.5; color: split.theme.muted; opacity: 0.45 }
    }
}
