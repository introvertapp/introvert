import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
Item {
    id: browser
    required property var theme
    property bool preview: false
    property var segmentManager: null
    readonly property var showRecords: preview ? shows : segmentManager ? segmentManager.shows : []
    readonly property var seasonNumbers: preview ? [0,1,2] : segmentManager ? segmentManager.seasons : []
    readonly property var episodeRecords: preview ? [] : segmentManager && selectedSeason>=0 ? segmentManager.episodes.filter(function(e){return e.season===browser.seasonNumbers[browser.selectedSeason]}) : []
    onSelectedShowChanged: { if(!preview && segmentManager) segmentManager.browseShow(selectedShow>=0 && selectedShow<showRecords.length ? showRecords[selectedShow].id : "") }
    function resetSelection() { selections=[[],[],[]]; anchorsByColumn=[-1,-1,-1] }
    function deletionTargets() {
        if(selectedShow>=0 && selections[2].length) return [{showId:showRecords[selectedShow].id,seasonNumber:seasonNumbers[selectedSeason],episodeNumbers:selections[2].map(function(i){return browser.episodeRecords[i].number})}]
        if(selectedShow>=0 && selections[1].length) return selections[1].map(function(i){return {showId:browser.showRecords[browser.selectedShow].id,seasonNumber:browser.seasonNumbers[i]}})
        return selections[0].map(function(i){return {showId:browser.showRecords[i].id}})
    }
    Connections { target: browser.segmentManager || null; function onDeleted() { browser.resetSelection() } }
    readonly property string helperText: {
        var count = selections[2].length || selections[1].length || selections[0].length
        var noun = selections[2].length ? "episode" : selections[1].length ? "season" : "TV show"
        return count ? "All segment timestamps for " + noun + (count===1 ? "" : "s") + " will be deleted" : "Select a TV show, season, or episode to delete its segment timestamps"
    }
    property var shows: []
    property var selections: [[], [], []]
    property var anchorsByColumn: [-1, -1, -1]
    readonly property int selectedShow: selections[0].length === 1 ? selections[0][0] : -1
    readonly property int selectedSeason: selections[1].length === 1 ? selections[1][0] : -1
    readonly property var selectedEpisodes: selections[2]
    function pick(column, index, modifiers) {
        var next = selections.map(function(values) { return values.slice() })
        if ((modifiers & Qt.ShiftModifier) && anchorsByColumn[column] >= 0) {
            if(!(modifiers & (Qt.ControlModifier | Qt.MetaModifier))) next[column] = []
            for (var n=Math.min(index,anchorsByColumn[column]); n<=Math.max(index,anchorsByColumn[column]); ++n) if(next[column].indexOf(n)<0) next[column].push(n)
        } else if (modifiers & (Qt.ControlModifier | Qt.MetaModifier)) {
            var at=next[column].indexOf(index)
            if(at>=0) next[column].splice(at,1); else next[column].push(index)
        } else next[column] = [index]
        for(var c=column+1;c<3;++c) next[c]=[]
        var anchors=anchorsByColumn.slice(); if(!(modifiers & Qt.ShiftModifier) || anchors[column]<0) anchors[column]=index; anchorsByColumn=anchors
        selections=next
    }
    function selectAll(column,count) {
        var next=selections.map(function(values){return values.slice()}); next[column]=[]
        for(var i=0;i<count;++i) next[column].push(i)
        for(var c=column+1;c<3;++c) next[c]=[]
        selections=next
    }
    objectName: "segmentDeletionBrowser"
    ColumnLayout {
        anchors.fill: parent; spacing: 12
        SplitView {
            objectName: "segmentColumns"
            Layout.fillWidth: true; Layout.fillHeight: true; orientation: Qt.Horizontal
            handle: Item {
                implicitWidth: 10
                Rectangle { anchors.centerIn: parent; width: 3; height: 22; radius: 1.5; color: browser.theme.muted; opacity: 0.45 }
            }
            Repeater {
                model: ["TV Shows", "Seasons", "Episodes"]
                Rectangle {
                    id: column
                    required property string modelData
                    required property int index
                    SplitView.fillWidth: true; SplitView.minimumWidth: 110; SplitView.preferredWidth: browser.width / 3
                    radius: 8; color: browser.theme.field; border.color: browser.theme.border
                    ColumnLayout {
                        anchors.fill: parent; anchors.margins: 8
                        Text { text: column.modelData.toUpperCase(); font.pixelSize: 13; color: browser.theme.muted; font.bold: true }
                        ListView {
                            id: list
                            objectName: "segmentList_" + column.index
                            Layout.fillWidth: true; Layout.fillHeight: true; clip: true
                            model: column.index === 0 ? browser.showRecords.map(function(s){return s.title}) : column.index === 1 ? browser.selectedShow >= 0 ? browser.seasonNumbers.map(function(n){return "Season " + n}) : [] : browser.selectedSeason >= 0 ? browser.preview ? ["E01 · First Light", "E02 · The Signal", "E03 · Crossing Paths", "E04 · A Quiet Place"] : browser.episodeRecords.map(function(e){return "E"+String(e.number).padStart(2,"0")+" · "+e.title}) : []
                            focus: true
                            Keys.onPressed: function(event) { if ((event.modifiers & (Qt.ControlModifier | Qt.MetaModifier)) && event.key === Qt.Key_A) { browser.selectAll(column.index,list.count); event.accepted = true } }
                            delegate: ItemDelegate {
                                HoverHandler { cursorShape: Qt.PointingHandCursor }
                                id: option
                                required property string modelData
                                required property int index
                                objectName: "segmentOption_" + column.index + "_" + index
                                width: list.width; height: 33; text: modelData; font.pixelSize: 14
                                hoverEnabled: true
                                highlighted: browser.selections[column.index].indexOf(index) >= 0
                                background: Rectangle { radius: 5; color: option.highlighted ? browser.theme.selected : option.hovered ? browser.theme.hover : "transparent"; border.color: "transparent" }
                                contentItem: Text { text: option.text; leftPadding: 8; verticalAlignment: Text.AlignVCenter; color: browser.theme.text; font.pixelSize: 14; elide: Text.ElideRight }
                                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onPressed: function(mouse) { browser.pick(column.index,option.index,mouse.modifiers); list.forceActiveFocus() } }

                            }
                            Column { visible: Boolean(column.index===1 && browser.segmentManager && browser.segmentManager.busy); width: list.width; spacing: 8; Repeater { model: 3; Skeleton { width: list.width; height: 24; theme: browser.theme } } }
                            Text { visible: list.count === 0 && !(column.index===1 && browser.segmentManager && browser.segmentManager.busy); width: parent.width; y: 25; text: column.index === 0 ? "No library connected" : column.index === 1 && browser.segmentManager && browser.segmentManager.busy && browser.selectedShow>=0 ? "Loading seasons..." : column.index === 1 ? browser.selections[0].length>1 ? "Select one TV show to browse seasons." : "Select a TV show." : browser.selections[1].length>1 ? "Select one season to browse episodes." : "Select a season."; color: browser.theme.muted; wrapMode: Text.WordWrap; font.pixelSize: 13 }
                        }
                    }
                }
            }
        }
        RowLayout {
            AppButton { objectName: "deleteSegmentsControl"; destructive: true; text: "Delete segment timestamps"; enabled: browser.selections[0].length > 0 && (browser.preview || Boolean(browser.segmentManager) && !browser.segmentManager.busy && browser.segmentManager.canDelete(browser.deletionTargets())); theme: browser.theme; onClicked: confirm.open() }

        }
        Text { visible: Boolean(browser.segmentManager) && browser.segmentManager.message.length>0; Layout.fillWidth: true; text: browser.segmentManager ? browser.segmentManager.message : ""; color: browser.theme.muted; wrapMode: Text.WordWrap; font.pixelSize: 13 }
        Text { Layout.fillWidth: true; text: browser.helperText; color: browser.theme.muted; font.pixelSize: 13; wrapMode: Text.WordWrap }
    }
    ActionDialog { id: confirm; objectName: "segmentDeletionConfirmation"; theme: browser.theme; title: "Delete segment timestamps?"; confirmText: browser.preview ? "Delete (preview only)" : "Delete"; message: browser.preview ? "The original operation cannot be undone. This is a design preview: no stored timestamps will be deleted." : browser.helperText + ". This operation cannot be undone."; onConfirmed: { if(!browser.preview && browser.segmentManager) browser.segmentManager.deleteBulk(browser.deletionTargets()) } }
}
