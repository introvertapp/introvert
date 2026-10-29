import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: page
    property var theme
    required property var appSettings
    required property var appNavigation
    property bool preview: false
    property var mediaLibrary: null
    property real headerInset: 0
    property alias glassHost: libraryGlassHost
    property alias scrollSurface: libraryContent
    property var items: []
    property string query: ""
    property string visualState: "ready"
    property string filterQuery: ""
    onQueryChanged: filterDelay.restart()
    Timer { id: filterDelay; interval: 70; onTriggered: page.filterQuery=page.query.toLowerCase() }
    readonly property var visibleItems: (visualState === "ready" || (!preview && items.length > 0 && visualState === "loading")) ? items.filter(function(s) { return (s.title + (page.preview ? " first light s01e01" : "")).toLowerCase().indexOf(filterQuery) >= 0 }) : []
    signal showOpened(var show)
    objectName: "libraryPage"
    Component {
        id: summaryHeader
        // Reserve the initial toolbar/stats space inside each scrolling view.
        // The real summary is a fixed overlay, not part of this scroll header.
        Item { width: page.width; height: page.headerInset+91 }
    }
    Item {
        id: libraryContent
        anchors.fill: parent; anchors.topMargin: -page.headerInset
        ListModel { id: posters; dynamicRoles: true }
        function syncPosters() {
            var rows=page.visibleItems, current={}
            for(var n=0;n<rows.length;n++) current[String(rows[n].id)]=true
            for(var n=posters.count-1;n>=0;n--) if(!current[String(posters.get(n).record.id)]) posters.remove(n)
            for(var n=0;n<rows.length;n++) {
                if(n>=posters.count) posters.append({record:rows[n]})
                else if(String(posters.get(n).record.id)!==String(rows[n].id)) posters.insert(n,{record:rows[n]})
                else if(JSON.stringify(posters.get(n).record)!==JSON.stringify(rows[n])) posters.setProperty(n,"record",rows[n])
            }
            if(posters.count>rows.length) posters.remove(rows.length,posters.count-rows.length)
        }
        Connections { target: page; function onVisibleItemsChanged() { libraryContent.syncPosters() } }
        Component.onCompleted: syncPosters()
        GridView {
            id: gallery
            objectName: "posterGallery"
            visible: page.appSettings.listingType !== "basic"
            anchors.fill: parent; anchors.rightMargin: -Math.ceil(columnGap)
            clip: true
            header: visible ? summaryHeader : null
            // Round the trailing viewport gap up so fractional cell spacing
            // cannot push the last column onto another row on HiDPI displays.
            readonly property real posterWidth: 160
            property int columns: Math.max(1,Math.floor((page.width+24)/184))
            readonly property real columnGap: columns>1 ? (page.width-columns*posterWidth)/(columns-1) : 24
            cellWidth: posterWidth+columnGap; cellHeight: posterWidth*1.5+24
            cacheBuffer: cellHeight
            model: posters
            delegate: PosterTile {
                required property var model
                record: model.record
                width: gallery.posterWidth; height: width*1.5
                theme: page.theme
                matchText: page.preview && page.query && record.title.toLowerCase().indexOf(page.filterQuery)<0 ? "S01E01 · First Light" : ""
                onClicked: page.showOpened(record)
            }
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AlwaysOff }
            Grid {
                visible: page.visualState === "loading" && !posters.count
                y: -gallery.contentY
                width: gallery.width; columns: gallery.columns; columnSpacing: gallery.columnGap; rowSpacing: 24
                Repeater { model: 12; Skeleton { width: gallery.posterWidth; height: width*1.5; theme: page.theme } }
            }
        }
        ListView {
            objectName: "basicLibraryList"
            visible: page.appSettings.listingType === "basic"
            anchors.fill: parent; clip: true; spacing: 10
            header: visible ? summaryHeader : null
            model: page.visibleItems
            delegate: ItemDelegate {
                HoverHandler { cursorShape: Qt.PointingHandCursor }
                required property var modelData
                width: ListView.view.width; height: 70
                contentItem: RowLayout {
                    spacing: 14
                    Text { text: modelData.title; color: page.theme.text; font.pixelSize: 16; font.bold: true; Layout.fillWidth: true; elide: Text.ElideRight }
                    Text { text: modelData.year + "   ·   " + modelData.seasons + " seasons   ·   " + modelData.episodes + " episodes"; color: page.theme.muted; font.pixelSize: 13 }
                }
                padding: 14
                onClicked: page.showOpened(modelData)
                background: Rectangle { color: parent.hovered ? page.theme.hover : page.theme.card; border.color: page.theme.border; radius: 12 }
            }
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AlwaysOff }
        }
    }
    Item {
        id: libraryGlassHost
        objectName: "libraryGlassHost"
        z: 1
        x: -20; y: -page.headerInset
        width: page.width+40; height: page.headerInset+67+12
    }
    Item {
        objectName: "fixedLibrarySummary"
        z: 2
        anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
        height: 67
        StatCards {
            objectName: "librarySummary"
            anchors.fill: parent; gap: 24; theme: page.theme
            entries: [{label:"Media Server",value:page.preview ? "Plex" : page.mediaLibrary ? page.mediaLibrary.status : page.appSettings.mediaProvider || "Not configured"}, {label:"Shows",value:page.preview || page.mediaLibrary ? page.items.length : 0}, {label:"Episodes",value:page.preview ? "150" : page.mediaLibrary ? page.mediaLibrary.episodeCount : 0}]
        }
        // Keep clicks/hover off the artwork beneath these non-interactive cards,
        // while allowing wheel events to reach the scrolling view underneath.
        MouseArea { anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.ArrowCursor; onWheel: function(wheel) { wheel.accepted=false } }
    }
    Column {
        visible: page.visibleItems.length === 0 && page.visualState !== "loading"
        anchors.left: parent.left; anchors.top: parent.top; anchors.topMargin: 95; width: parent.width; spacing: 12

        Text {
            width: parent.width; horizontalAlignment: Text.AlignLeft; wrapMode: Text.WordWrap
            text: page.visualState === "loading" ? "Loading TV shows…" : page.visualState === "error" ? "Unable to load library" : page.query ? "No shows found." : page.preview ? "No shows found." : page.mediaLibrary && page.appSettings.mediaProvider !== "" ? "No TV shows found." : "Connect a media library"
            font.pixelSize: 15; color: page.theme.muted
        }
        Text { visible: !page.preview; width: parent.width; horizontalAlignment: Text.AlignLeft; text: page.preview ? "Design preview — no provider requests are made." : page.mediaLibrary ? page.mediaLibrary.message : "Configure a media provider in Settings to browse your library."; wrapMode: Text.WordWrap; color: page.theme.muted }
        AppButton { visible: !page.preview; anchors.left: parent.left; theme: page.theme; text: page.preview ? "Reset preview state" : "Configure Media"; onClicked: page.preview ? page.visualState = "ready" : page.appNavigation.navigate("settings") }
    }
}
