import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
Item {
    id: page
    required property var theme
    property var show: null
    property real headerInset: 0
    property alias glassHost: detailGlassHost
    property alias scrollSurface: episodeList
    readonly property var externalLinks: {
        var ids = page.show && page.show.externalIds ? page.show.externalIds : {}
        var imdb = ids.imdb && ids.imdb !== "--" ? String(ids.imdb).trim() : ""
        var tmdb = ids.tmdb ? String(ids.tmdb).trim() : ""
        var links = []
        if(tmdb) links.push({id:"tmdb", label:"TMDB", href:"https://www.themoviedb.org/tv/"+encodeURIComponent(tmdb), logo:"tmdb_logo.svg"})
        if(imdb) links.push({id:"imdb", label:"IMDB", href:"https://www.imdb.com/title/"+encodeURIComponent(imdb)+"/", logo:"imdb_logo.svg"})
        if(tmdb) links.push({id:"theintrodb-org", label:"TheIntroDB", href:"https://theintrodb.org/tv/"+encodeURIComponent(tmdb), logo:"theintrodb_org_logo.svg"})
        if(imdb) links.push({id:"introdb-app", label:"IntroDB", href:"https://introdb.app/shows/"+encodeURIComponent(imdb), logo:"introdb_app_logo.svg"})
        return links
    }
    property var segmentManager: null
    property var submissions: null
    property var detector: null
    property var episodes: []
    property bool preview: false
    property bool loading: false
    property string errorMessage: ""
    signal seasonRequested(int number)
    property int season: 1
    property string timestampFormat: "both"
    property var selectionByEpisode: ({})
    readonly property int selectedCount: Object.keys(selectionByEpisode).filter(function(id){return selectionByEpisode[id].length>0}).length
    readonly property int selectedSegmentCount: Object.keys(selectionByEpisode).reduce(function(n,id){return n+selectionByEpisode[id].length},0)
    readonly property var selectedTypes: {
        var counts={}
        Object.keys(selectionByEpisode).forEach(function(id){selectionByEpisode[id].forEach(function(kind){counts[kind]=(counts[kind] || 0)+1})})
        return counts
    }
    function hasKind(kind) {return episodes.some(function(ep){return ep.segmentValues ? Boolean(ep.segmentValues[kind.toLowerCase()]) : ep[kind.toLowerCase()] && ep[kind.toLowerCase()]!=="—"})}
    function selectKind(kind,checked) {
        var next=Object.assign({},selectionByEpisode)
        episodes.forEach(function(ep){
            var kinds=(next[ep.id] || []).filter(function(value){return value!==kind})
            if(checked && (ep.segmentValues ? ep.segmentValues[kind.toLowerCase()] : ep[kind.toLowerCase()] && ep[kind.toLowerCase()]!=="—")) kinds.push(kind)
            next[ep.id]=kinds
        })
        selectionByEpisode=next
    }
    function selectedPayload() {return episodes.filter(function(ep){return (selectionByEpisode[ep.id] || []).length>0}).map(function(ep){return {id:ep.id,segmentTypes:selectionByEpisode[ep.id].map(function(kind){return kind.toLowerCase()})}})}
    function scanStateFor(ep) {
        if(!detector || !detector.status.id)return ""
        var state=detector.status, kinds=JSON.parse(state.kinds || "[]")
        if(detector.active) return "PROCESSING"
        if(state.status==="failed")return "FAILED"
        if(state.status==="cancelled")return "CANCELLED"
        var result=JSON.parse(state.result || "{}"), applied=result.updated ? result.updated[ep.id] : null
        if(applied) return Object.keys(applied).filter(function(kind){
            var current=ep.segmentValues && ep.segmentValues[kind], detected=applied[kind]
            return current && detected && current[0]===detected[0] && current[1]===detected[1]
        }).map(function(kind){return kind.toUpperCase()}).join("/")
        if(result.eligible && result.eligible[ep.id] && result.eligible[ep.id].length) return "NO "+result.eligible[ep.id].map(function(kind){return kind.toUpperCase()}).join("/")
        return ""
    }
    function submissionStateFor(ep) {return submissions ? submissions.episodeStatuses[(ep.providerId || (show ? show.provider : ""))+":"+(ep.showId || (show ? show.id : ""))+":"+ep.id] || {} : {}}
    property string endpoint: submissions && submissions.endpoints.length ? submissions.endpoints[0] : "introdb.app"
    function clearSelection() { selectionByEpisode={} }
    onShowChanged: {if(!show || show.id!==selectionShowId) { clearSelection();selectionShowId=show ? String(show.id || "") : "";resetScroll() }}
    property string selectionShowId: ""
    function resetScroll() {Qt.callLater(function(){
        episodeList.forceLayout();episodeList.positionViewAtBeginning()
        // Include the reserved frosted-header space in the initial viewport.
        if(episodeList.headerItem) episodeList.contentY=episodeList.headerItem.y
    })}
    onSeasonChanged: {clearSelection();resetScroll()}
    onLoadingChanged: if(!loading) resetScroll()
    signal backRequested()
    signal editorRequested(string episodeTitle, var episode)
    signal reviewRequested()
    signal submissionErrorRequested(string title, string message)
    signal unavailableRequested(string title)
    objectName: "showDetailPage"
    ListView {
        id: episodeList
        objectName: "episodeList"
        anchors.fill: parent; anchors.topMargin: -page.headerInset; clip: true; spacing: 10; cacheBuffer: 500
        currentIndex: -1; highlightFollowsCurrentItem: false
        onVisibleChanged: if(visible) page.resetScroll()
        onCountChanged: if(count>0) page.resetScroll()
        model: page.loading ? [] : page.episodes
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AlwaysOff }
        header: Column {
            width: episodeList.width; spacing: 10; bottomPadding: 10
            height: Math.max(page.headerInset+154,implicitHeight)
            Item { width: parent.width; height: page.headerInset+144 }

            Column {
                visible: page.loading; width: parent.width; spacing: 10
                Repeater { model: page.loading ? 3 : 0; Skeleton { width: episodeList.width; height: 140; theme: page.theme } }
            }
            Text { visible: page.errorMessage!=="" || Boolean(page.submissions && page.submissions.message); text: page.submissions && page.submissions.message ? page.submissions.message : page.errorMessage; color: page.theme.muted; width: parent.width; wrapMode: Text.WordWrap }
        }
        delegate: EpisodeItem {
            id: episodeDelegate
            required property var modelData
            width: episodeList.width; height: implicitHeight
            theme: page.theme; episode: modelData; segmentManager: page.segmentManager; preview: page.preview; timestampFormat: page.timestampFormat
            selectedKinds: page.selectionByEpisode[modelData.id] || []
            submissionState: page.submissionStateFor(modelData); scanState: page.scanStateFor(modelData)
            onEditorRequested: page.editorRequested("S"+page.season+"・E"+modelData.number+" - "+modelData.title,modelData)
            onSubmissionErrorRequested: function(title,message) {page.submissionErrorRequested(title,message)}
            onSelectionChanged: {if(JSON.stringify(page.selectionByEpisode[modelData.id] || [])!==JSON.stringify(selectedKinds)) {var next=Object.assign({},page.selectionByEpisode);next[modelData.id]=selectedKinds;page.selectionByEpisode=next}}
            Connections { target: page; function onSelectionByEpisodeChanged() {episodeDelegate.selectedKinds=page.selectionByEpisode[episodeDelegate.modelData.id] || []} }
        }
        footer: Column {
            width: episodeList.width; spacing: 10; topPadding: 20
            RowLayout {
                Repeater { model: ["Recap","Intro","Credits","Preview"]; SegmentSwitch { compact: true; required property string modelData; objectName: "selectAll_"+modelData; text: modelData; enabled: page.hasKind(modelData); checked: enabled && page.episodes.every(function(ep){return !(ep.segmentValues ? ep.segmentValues[modelData.toLowerCase()] : ep[modelData.toLowerCase()] && ep[modelData.toLowerCase()]!=="—") || (page.selectionByEpisode[ep.id] || []).indexOf(modelData)>=0}); onToggled: page.selectKind(modelData,checked) } }
            }
            RowLayout {
                AppComboBox { id: endpointPicker; onActivated: page.endpoint=currentText; model: page.submissions ? page.submissions.endpoints : ["introdb.app", "theintrodb.org"]; Layout.preferredWidth: 180 }
                AppButton { objectName: "reviewSubmissionControl"; text: page.selectedCount > 0 ? "Submit (" + page.selectedCount + ")" : "Submit"; enabled: page.selectedCount > 0 && (!page.submissions || !page.submissions.busy); theme: page.theme; onClicked: page.reviewRequested() }
            }

        }
    }
    Item {
        id: detailGlassHost
        objectName: "detailGlassHost"
        z: 1
        x: -20; y: -page.headerInset
        width: page.width+40; height: page.headerInset+144+10
    }
    Item {
        objectName: "fixedShowOverview"
        z: 2
        width: parent.width; height: 144
        RowLayout {
            anchors.left: parent.left; anchors.top: parent.top; spacing: 18
            Item {
                Layout.preferredWidth: 250; Layout.minimumWidth: 250; Layout.maximumWidth: 250; Layout.preferredHeight: 88
                AppImage {
                    id: logo
                    objectName: "showLogo"
                    anchors.fill: parent; source: page.show ? page.show.logoUrl || "" : ""
                    fillMode: Image.PreserveAspectFit; horizontalAlignment: Image.AlignLeft
                    // Decode for the largest header slot, not every resize step.
                    sourceSize.width: Math.ceil(250*Screen.devicePixelRatio*2)
                    sourceSize.height: Math.ceil(88*Screen.devicePixelRatio*2)
                }
                Text { objectName: "showTitleFallback"; visible: logo.status === Image.Null || logo.status === Image.Error; anchors.fill: parent; text: page.show ? page.show.title : ""; font.pixelSize: 32; font.bold: true; color: page.theme.text; wrapMode: Text.WordWrap; verticalAlignment: Text.AlignVCenter }
            }
            ColumnLayout {
                Text { text: page.show ? page.show.year + " • " + page.show.episodes + " episodes • " + page.show.seasons + " seasons" : ""; color: page.theme.muted; font.bold: true; font.pixelSize: 14 }
                Row {
                    spacing: 9
                    Repeater {
                        model: page.externalLinks
                        AppImage {
                            id: externalLogo
                            required property var modelData
                            source: "assets/"+modelData.logo
                            height: modelData.id === "theintrodb-org" ? 13 : modelData.id === "introdb-app" ? 18 : 20
                            width: Math.min(modelData.id === "theintrodb-org" ? 58 : modelData.id === "introdb-app" ? 26 : Infinity, implicitHeight > 0 ? implicitWidth*height/implicitHeight : 0)
                            y: (20-height)/2; fillMode: Image.PreserveAspectFit
                            MouseArea {
                                id: externalMouse
                                objectName: "externalLink_"+externalLogo.modelData.id
                                anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; activeFocusOnTab: true
                                Keys.onReturnPressed: Qt.openUrlExternally(externalLogo.modelData.href)
                                Accessible.role: Accessible.Link; Accessible.name: "Open "+externalLogo.modelData.label
                                onClicked: Qt.openUrlExternally(externalLogo.modelData.href)
                            }
                            AppToolTip { visible: externalMouse.containsMouse; text: "Open "+externalLogo.modelData.label; theme: page.theme }
                        }
                    }
                }
            }
        }
        Flow {
            anchors.left: parent.left; anchors.bottom: parent.bottom; width: parent.width-200; spacing: 10
            Repeater {
                model: page.show ? page.show.seasonNumbers || Array.from({length:page.show.seasons || 0},function(_,index){return index+1}) : []
                AppButton { required property var modelData; objectName: "season_" + modelData; text: "Season " + modelData; fontSize: 11; implicitHeight: 31; selected: page.season === modelData; theme: page.theme; onClicked: { if(page.preview) page.season = modelData; page.seasonRequested(modelData) } }
            }
        }
        ColumnLayout {
            anchors.right: parent.right; anchors.bottom: parent.bottom; spacing: 7
            RowLayout { visible: !page.detector || !page.detector.active; SegmentSwitch { id: detectIntro; objectName: "detectIntroToggle"; compact: true; text: "Intro"; checked: true } SegmentSwitch { id: detectCredits; objectName: "detectCreditsToggle"; compact: true; text: "Credits"; checked: true } }
            AppButton { objectName: "detectSegmentsControl"; Layout.fillWidth: true; progress: page.detector && page.detector.active ? Number(page.detector.status.current || 0)/Math.max(1,Number(page.detector.status.total || 0)) : -1; text: page.detector && page.detector.active ? "Stop Detection" : "Detect Segments"; enabled: Boolean(page.detector) && !page.loading; implicitHeight: 30; fontSize: 12; theme: page.theme; onClicked: { if(page.detector.active) page.detector.stop(); else page.detector.start((detectIntro.checked ? ["intro"] : []).concat(detectCredits.checked ? ["credits"] : [])) } }
        }
    }

}
