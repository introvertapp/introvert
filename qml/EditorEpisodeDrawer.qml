import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "SegmentTime.js" as SegmentTime

Item {
    id: drawer
    required property var editor
    required property var theme
    property real reveal: 1
    property real layoutWidth: 280
    onWidthChanged: if(width>layoutWidth)layoutWidth=width
    clip: true
    readonly property var seasons: editor.backend && !editor.preview ? editor.backend.seasons : editor.previewSeasonNumbers
    readonly property var episodes: editor.backend && !editor.preview ? editor.backend.episodes : editor.previewEpisodes
    readonly property int season: editor.backend && !editor.preview ? editor.backend.season : editor.previewSeason
    readonly property string currentId: editor.backend && !editor.preview ? editor.backend.episodeId : editor.previewEpisodeId
    function ranges(episode) {
        var rows=[]
        for(var i=0;i<editor.segmentTemplates.length;i++) {
            var kind=editor.segmentTemplates[i],key=kind.label.toLowerCase(),pair=episode.segmentValues ? episode.segmentValues[key] : null
            if(!pair && editor.preview && episode[key] && episode[key]!=="—") {
                var ends=episode[key].split(/\s*[–→]\s*/)
                function parse(value) { return value.split(":").reduce(function(total,part){return total*60+Number(part)},0) }
                if(ends.length===2)pair=[parse(ends[0]),parse(ends[1])]
            }
            if(pair)rows.push({label:kind.label,color:kind.color,text:SegmentTime.lines(pair[0],pair[1],editor.timestampFormat)})
        }
        return rows
    }
    ColumnLayout {
        objectName: "editorEpisodeContents"
        opacity: drawer.reveal
        x: 8; y: 8
        width: drawer.layoutWidth-16; height: Math.max(0,drawer.height-16); spacing: 8
        AppComboBox {
            objectName: "editorSeasonControl"
            // Keep episode text stable while allowing this control to shrink.
            Layout.alignment: Qt.AlignLeft
            Layout.preferredWidth: Math.max(0,drawer.width-16)
            Layout.maximumWidth: Math.max(0,drawer.width-16)
            implicitHeight: 40
            model: drawer.seasons.map(function(number){return number===0 ? "Specials" : number<0 ? "Episodes" : "Season "+number})
            currentIndex: drawer.seasons.indexOf(drawer.season)
            enabled: drawer.seasons.length>0
            onActivated: function(index) {
                if(drawer.editor.backend && !drawer.editor.preview)drawer.editor.backend.selectSeason(drawer.seasons[index])
                else drawer.editor.previewSeason=drawer.seasons[index]
            }
        }
        ListView {
            id: episodeList
            objectName: "editorEpisodeList"
            Layout.fillWidth: true; Layout.fillHeight: true
            clip: true; spacing: 6; boundsBehavior: Flickable.StopAtBounds
            model: drawer.episodes
            currentIndex: drawer.episodes.map(function(episode){return episode.id}).indexOf(drawer.currentId)
            highlightFollowsCurrentItem: false
            function revealCurrent() { if(currentIndex>=0)positionViewAtIndex(currentIndex,ListView.Contain) }
            onCurrentIndexChanged: Qt.callLater(revealCurrent)
            onModelChanged: Qt.callLater(revealCurrent)
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AlwaysOff }
            delegate: Item {
                id: episodeRow
                required property var modelData
                readonly property var ranges: drawer.ranges(modelData)
                objectName: "editorEpisode_"+modelData.id
                width: episodeList.width; height: contents.implicitHeight+16
                Rectangle { anchors.fill: parent; radius: 5; color: drawer.currentId===episodeRow.modelData.id ? drawer.theme.selected : hover.hovered ? drawer.theme.hover : drawer.theme.field }
                HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
                TapHandler {
                    enabled: !drawer.editor.backend || !drawer.editor.backend.busy
                    onTapped: drawer.editor.requestEpisode(episodeRow.modelData)
                }
                Accessible.role: Accessible.Button
                Accessible.name: (modelData.number==null ? "Episode" : "Episode "+modelData.number)+" "+modelData.title
                ColumnLayout {
                    id: contents
                    x: 8; y: 8; width: Math.max(0,parent.width-16); spacing: 6
                    Text { Layout.fillWidth: true; text: (episodeRow.modelData.number==null ? "" : episodeRow.modelData.number+". ")+episodeRow.modelData.title; font.pixelSize: 14; font.bold: true; color: drawer.theme.text; wrapMode: Text.WordWrap }
                    Repeater {
                        model: episodeRow.ranges
                        ColumnLayout {
                            required property var modelData
                            Layout.fillWidth: true; spacing: 2
                            Text { text: modelData.label; color: drawer.theme.muted; font.pixelSize: 12; font.weight: Font.DemiBold }
                            Text { objectName: "editorTimestamp_"+episodeRow.modelData.id+"_"+modelData.label; Layout.fillWidth: true; text: modelData.text; font.pixelSize: 12; color: modelData.color; wrapMode: Text.Wrap }
                        }
                    }
                    Text { visible: !episodeRow.ranges.length; text: "No segment timestamps"; color: drawer.theme.muted; font.pixelSize: 12; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                }
            }
            Text { anchors.centerIn: parent; visible: !episodeList.count; text: "No episodes"; color: drawer.theme.muted; font.pixelSize: 13 }
        }
    }
}
