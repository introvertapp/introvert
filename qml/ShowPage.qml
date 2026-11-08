import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import "SegmentTime.js" as SegmentTime
Item {
    id: page
    required property var theme
    property var show: null
    property bool movieMode: false
    property real headerInset: 0
    property alias glassHost: detailGlassHost
    property alias scrollSurface: episodeList
    readonly property var externalLinks: {
        var ids = page.show && page.show.externalIds ? page.show.externalIds : {}
        var imdb = ids.imdb && ids.imdb !== "--" ? String(ids.imdb).trim() : ""
        var tmdb = ids.tmdb ? String(ids.tmdb).trim() : ""
        var tvdb = ids.tvdb ? String(ids.tvdb).trim() : ""
        var links = []
        if(tmdb && tmdb !== "--") links.push({id:"tmdb", label:"TMDB", href:"https://www.themoviedb.org/"+(page.movieMode ? "movie/" : "tv/")+encodeURIComponent(tmdb), logo:"tmdb_logo.svg"})
        if(imdb) links.push({id:"imdb", label:"IMDB", href:"https://www.imdb.com/title/"+encodeURIComponent(imdb)+"/", logo:"imdb_logo.svg"})
        if(tvdb && tvdb !== "--") links.push({id:"theintrodb-org", label:"TheIntroDB", href:"https://theintrodb.org/"+(page.movieMode ? "movie/" : "tv/")+encodeURIComponent(tvdb), logo:"theintrodb_org_logo.svg"})
        if(imdb) links.push({id:"introdb-app", label:"IntroDB", href:"https://introdb.app/shows/"+encodeURIComponent(imdb), logo:"introdb_app_logo.svg"})
        return links
    }
    property var segmentManager: null
    property var localMetadata: null
    property var submissions: null
    property var detector: null
    property var episodes: []
    property var mediaLibrary: null
    readonly property var submissionEpisodes: {
        var currentEpisodes=episodes // Reevaluate the catalog after segment edits and refreshes.
        if(!detectionSeasons.length || !mediaLibrary) return currentEpisodes
        var rows=[]
        requestedDetectionSeasons.forEach(function(number){rows=rows.concat(mediaLibrary.episodesForSeason(number))})
        return rows
    }
    property bool preview: false
    property bool loading: false
    property string errorMessage: ""
    signal seasonRequested(int number)
    property int season: 1
    property string timestampFormat: "both"
    property var selectionByEpisode: ({})
    property var detectionSeasons: []
    readonly property var requestedDetectionSeasons: detectionSeasons.length ? detectionSeasons : [season]
    readonly property var detectionRun: detector ? detector.showDetection : ({})
    readonly property bool requestedDetectionActive: Boolean(detectionRun.active)
    readonly property real requestedDetectionProgress: detectionRun.progress === undefined ? -1 : Number(detectionRun.progress)
    function seasonDetectionProgress(number) {
        var state = detector ? detector.seasonProgress[String(number)] : null
        return state && ["queued", "running", "complete"].indexOf(state.status) >= 0 ? Number(state.progress || 0) : -1
    }
    function toggleDetectionSeason(number) {
        if(!detectionSeasons.length && number !== season) { detectionSeasons = [season, number]; return }
        detectionSeasons = detectionSeasons.indexOf(number) >= 0 ? detectionSeasons.filter(function(value) { return value !== number }) : detectionSeasons.concat([number])
    }
    function openSeason(number) { if(preview) season = number; seasonRequested(number) }
    readonly property int selectedCount: selectedPayload().length
    readonly property int selectedSegmentCount: selectedPayload().reduce(function(n,item){return n+item.segmentTypes.length},0)
    readonly property var selectedTypes: {
        var counts={}
        selectedPayload().forEach(function(item){item.segmentTypes.forEach(function(kind){var label=kind.charAt(0).toUpperCase()+kind.slice(1);counts[label]=(counts[label] || 0)+1})})
        return counts
    }
    function hasKind(kind) {return submissionEpisodes.some(function(ep){return ep.segmentValues ? Boolean(ep.segmentValues[kind.toLowerCase()]) : ep[kind.toLowerCase()] && ep[kind.toLowerCase()]!=="—"})}
    function selectKind(kind,checked) {
        var next=Object.assign({},selectionByEpisode)
        submissionEpisodes.forEach(function(ep){
            var kinds=(next[ep.id] || []).filter(function(value){return value!==kind})
            if(checked && (ep.segmentValues ? ep.segmentValues[kind.toLowerCase()] : ep[kind.toLowerCase()] && ep[kind.toLowerCase()]!=="—")) kinds.push(kind)
            next[ep.id]=kinds
        })
        selectionByEpisode=next
    }
    function selectedPayload() {return submissionEpisodes.filter(function(ep){return (selectionByEpisode[ep.id] || []).length>0}).map(function(ep){return {id:ep.id,segmentTypes:selectionByEpisode[ep.id].map(function(kind){return kind.toLowerCase()})}})}
    function scanStateFor(ep) {
        if(!detector || !detector.status.id)return ""
        var state=detector.status, kinds=JSON.parse(state.kinds || "[]")
        if(Number(state.status_revision || 0)<Number(ep.statusRevision || 0))return ""
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
    function submissionStateFor(ep) {
        var prefix = ep.movie ? "movie:" : ""
        var state=submissions ? submissions.episodeStatuses[prefix+(ep.providerId || (show ? show.provider : ""))+":"+(ep.showId || (show ? show.id : ""))+":"+ep.id] || {} : {}
        var revision=Number(ep.statusRevision || 0)
        if(!revision)return state
        var segments={},badges={}
        Object.keys(state.segments || {}).forEach(function(kind){
            if(Number(state.segments[kind].statusRevision || 0)>=revision)segments[kind]=state.segments[kind]
        })
        Object.keys(state.badges || {}).forEach(function(kind){
            var current=state.badges[kind].filter(function(badge){return Number(badge.statusRevision || 0)>=revision})
            if(current.length)badges[kind]=current
        })
        return {segments:segments,badges:badges}
    }
    property var selectedEndpoints: []
    readonly property var availableEndpoints: {
        var endpoints = submissions ? submissions.endpoints : ["introdb.app", "theintrodb.org"]
        if(!movieMode || preview) return endpoints
        var ids = show && show.externalIds ? show.externalIds : {}
        return ["theintrodb.org", "introdb.app"].filter(function(value) {
            return endpoints.indexOf(value)>=0 && (value==="introdb.app" ? ids.imdb : (ids.tvdb || ids.tmdb || ids.imdb))
        })
    }
    readonly property var requestedEndpoints: selectedEndpoints.length ? selectedEndpoints.filter(function(value){return availableEndpoints.indexOf(value)>=0}) : availableEndpoints.slice(0,1)
    readonly property string endpoint: requestedEndpoints.join(", ")
    function chooseEndpoint(value, multiple) {
        selectedEndpoints = multiple ? (requestedEndpoints.indexOf(value)>=0 ? requestedEndpoints.filter(function(item){return item!==value}) : requestedEndpoints.concat([value])) : [value]
    }
    function clearSelection() { selectionByEpisode={} }
    onShowChanged: {
        var key = show ? (movieMode ? "movie:" : "tv:") + String(show.id || "") : ""
        if(key !== selectionShowId) { clearSelection(); detectionSeasons=[]; selectionShowId=key; resetScroll() }
    }
    property string selectionShowId: ""
    function resetScroll() {Qt.callLater(function(){
        episodeList.forceLayout();episodeList.positionViewAtBeginning()
        // Include the reserved frosted-header space in the initial viewport.
        if(episodeList.headerItem) episodeList.contentY=episodeList.headerItem.y
    })}
    onSeasonChanged: {resetScroll()}
    onLoadingChanged: if(!loading) resetScroll()
    signal backRequested()
    signal editorRequested(string episodeTitle, var episode)
    signal reviewRequested()
    signal submissionErrorRequested(string title, string message)
    signal unavailableRequested(string title)
    objectName: movieMode ? "movieDetailPage" : "showDetailPage"
    ListView {
        id: episodeList
        objectName: page.movieMode ? "movieItemList" : "episodeList"
        anchors.fill: parent; anchors.topMargin: -page.headerInset; clip: true; spacing: 10; cacheBuffer: 500
        currentIndex: -1; highlightFollowsCurrentItem: false
        onVisibleChanged: if(visible) page.resetScroll()
        onCountChanged: if(count>0) page.resetScroll()
        model: page.loading ? [] : page.episodes
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AlwaysOff }
        header: Column {
            width: episodeList.width; spacing: 10; bottomPadding: 10
            height: Math.max(page.headerInset+overview.height+10,implicitHeight)
            Item { width: parent.width; height: page.headerInset+overview.height }

            Column {
                visible: page.loading; width: parent.width; spacing: 10
                Repeater { model: page.loading ? (page.movieMode ? 1 : 3) : 0; Skeleton { width: episodeList.width; height: 140; theme: page.theme } }
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
            onEditorRequested: page.editorRequested(page.movieMode ? modelData.title : "S"+page.season+"・E"+modelData.number+" - "+modelData.title,modelData)
            onSubmissionErrorRequested: function(title,message) {page.submissionErrorRequested(title,message)}
            onSelectionChanged: {if(JSON.stringify(page.selectionByEpisode[modelData.id] || [])!==JSON.stringify(selectedKinds)) {var next=Object.assign({},page.selectionByEpisode);next[modelData.id]=selectedKinds;page.selectionByEpisode=next}}
            Connections { target: page; function onSelectionByEpisodeChanged() {episodeDelegate.selectedKinds=page.selectionByEpisode[episodeDelegate.modelData.id] || []} }
        }
        footer: Column {
            width: episodeList.width; spacing: 10; topPadding: 20
            RowLayout {
                Repeater { model: ["Recap","Intro","Credits","Preview"]; SegmentSwitch { compact: true; required property string modelData; objectName: "selectAll_"+modelData; text: modelData; enabled: page.hasKind(modelData); checked: enabled && page.submissionEpisodes.every(function(ep){return !(ep.segmentValues ? ep.segmentValues[modelData.toLowerCase()] : ep[modelData.toLowerCase()] && ep[modelData.toLowerCase()]!=="—") || (page.selectionByEpisode[ep.id] || []).indexOf(modelData)>=0}); onToggled: page.selectKind(modelData,checked) } }
            }
            RowLayout {
                Repeater {
                    model: page.availableEndpoints
                    AppButton {
                        id: endpointButton
                        required property string modelData
                        objectName: "endpoint_"+modelData
                        text: modelData === "introdb.app" ? "IntroDB" : modelData === "theintrodb.org" ? "TheIntroDB" : modelData; theme: page.theme; selected: page.requestedEndpoints.indexOf(modelData)>=0
                        onClicked: page.chooseEndpoint(modelData,false)
                        Keys.onSpacePressed: function(event) { page.chooseEndpoint(modelData,Boolean(event.modifiers & Qt.ShiftModifier)); event.accepted=true }
                        MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: function(mouse) { endpointButton.forceActiveFocus(); page.chooseEndpoint(endpointButton.modelData,Boolean(mouse.modifiers & Qt.ShiftModifier)) } }
                    }
                }
                AppButton { objectName: "reviewSubmissionControl"; progress: page.submissions ? page.submissions.progress : -1; text: page.selectedCount > 0 ? "Submit (" + page.selectedCount + ")" : "Submit"; enabled: page.requestedEndpoints.length>0 && page.selectedCount > 0 && (!page.submissions || !page.submissions.busy); theme: page.theme; onClicked: page.reviewRequested() }
                AppButton { objectName: "identifyLocalShowControl"; visible: Boolean(page.show && page.show.provider==="local" && page.localMetadata); text: "Identify"; enabled: Boolean(page.localMetadata) && !page.localMetadata.busy && !page.loading; theme: page.theme; onClicked: { page.localMetadata.activate(); providerShowId.text=""; identifyShow.open() } }
            }

        }
    }
    Item {
        id: detailGlassHost
        objectName: "detailGlassHost"
        z: 1
        x: -20; y: -page.headerInset
        width: page.width+40; height: page.headerInset+overview.height+10
    }
    Item {
        id: overview
        objectName: "fixedShowOverview"
        z: 2
        width: parent.width
        height: page.movieMode ? titleOverview.height + 10 : Math.max(144, titleOverview.height + 10 + seasonFlow.implicitHeight)
        onHeightChanged: {
            // A wrapping Flow settles after the first list layout. Keep the
            // initial card below the new header without jumping a scrolled list.
            if(page.visible && episodeList.contentY <= episodeList.originY) page.resetScroll()
        }
        RowLayout {
            id: titleOverview
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
                Text { objectName: "showTitleFallback"; visible: logo.status === Image.Null || logo.status === Image.Error; anchors.fill: parent; text: page.show ? page.show.title : ""; textFormat: Text.PlainText; font.pixelSize: 32; font.bold: true; color: page.theme.text; wrapMode: Text.WordWrap; verticalAlignment: Text.AlignVCenter }
            }
            ColumnLayout {
                Text { text: !page.show ? "" : page.movieMode ? SegmentTime.movieInfo(page.show) : page.show.year + " • " + page.show.episodes + " episodes • " + page.show.seasons + " seasons"; color: page.theme.muted; font.bold: true; font.pixelSize: 14 }
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
            id: seasonFlow
            visible: !page.movieMode
            anchors.left: parent.left; anchors.bottom: parent.bottom; width: parent.width-200; spacing: 10
            Repeater {
                model: page.show && !page.movieMode ? page.show.seasonNumbers || Array.from({length:page.show.seasons || 0},function(_,index){return index+1}) : []
                AppButton {
                    id: seasonButton
                    required property var modelData
                    readonly property bool wholeSeasonSelected: page.detectionSeasons.indexOf(Number(modelData)) >= 0
                    objectName: "season_" + modelData; text: "Season " + modelData; fontSize: 11; implicitHeight: 31
                    selected: page.season === modelData || wholeSeasonSelected
                    progress: page.requestedDetectionActive ? page.seasonDetectionProgress(Number(modelData)) : page.submissions && page.submissions.seasonProgress[String(modelData)] ? page.submissions.seasonProgress[String(modelData)].progress : page.seasonDetectionProgress(Number(modelData))
                    selectionColor: page.theme.selected
                    selectionBorderColor: page.theme.accent
                    theme: page.theme
                    onClicked: page.openSeason(Number(modelData))
                    Keys.onSpacePressed: function(event) {
                        if(event.modifiers & Qt.ShiftModifier) { page.toggleDetectionSeason(Number(modelData)); event.accepted = true }
                        else event.accepted = false
                    }
                    MouseArea {
                        anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                        onClicked: function(mouse) {
                            seasonButton.forceActiveFocus()
                            if(mouse.modifiers & Qt.ShiftModifier) page.toggleDetectionSeason(Number(seasonButton.modelData))
                            else page.openSeason(Number(seasonButton.modelData))
                        }
                    }
                }
            }
        }
        ColumnLayout {
            visible: !page.movieMode
            anchors.right: parent.right; anchors.bottom: parent.bottom; spacing: 7
            RowLayout { visible: !page.requestedDetectionActive; SegmentSwitch { id: detectIntro; objectName: "detectIntroToggle"; compact: true; text: "Intro"; checked: true } SegmentSwitch { id: detectCredits; objectName: "detectCreditsToggle"; compact: true; text: "Credits"; checked: true } }
            AppButton {
                objectName: "detectSegmentsControl"; Layout.fillWidth: true
                progress: page.requestedDetectionProgress
                text: page.requestedDetectionActive ? "Stop Detection" : page.detectionSeasons.length ? "Detect "+page.detectionSeasons.length+" Season"+(page.detectionSeasons.length===1 ? "" : "s") : "Detect Segments"
                enabled: Boolean(page.detector) && !page.loading; implicitHeight: 30; fontSize: 12; theme: page.theme
                onClicked: {
                    if(page.requestedDetectionActive) page.detector.stopSeasons(page.detectionRun.seasons || page.requestedDetectionSeasons)
                    else page.detector.startSeasons(page.requestedDetectionSeasons,(detectIntro.checked ? ["intro"] : []).concat(detectCredits.checked ? ["credits"] : []))
                }
            }
        }
    }

    Dialog {
        id: identifyShow; objectName: "identifyLocalShowDialog"; parent: Overlay.overlay; anchors.centerIn: parent
        modal: true; focus: true; padding: 20; width: Math.min(460,parent ? parent.width-40 : 460)
        title: "Identify show"
        background: Rectangle { color: page.theme.card; border.color: page.theme.border; radius: 14 }
        header: Text { text: identifyShow.title; padding: 20; font.pixelSize: 18; font.bold: true; color: page.theme.text }
        contentItem: ColumnLayout {
            spacing: 12
            Text { Layout.fillWidth: true; wrapMode: Text.WordWrap; color: page.theme.muted; text: "Enter the show ID from " + (page.localMetadata ? String(page.localMetadata.configuration.provider || "the selected provider") : "the selected provider") + ". Fetching will update the saved match." }
            AppTextField { id: providerShowId; objectName: "localShowProviderId"; Layout.fillWidth: true; placeholderText: "Provider show ID" }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                AppButton { text: "Cancel"; theme: page.theme; onClicked: identifyShow.close() }
                AppButton { objectName: "confirmLocalShowIdentification"; text: "Identify"; theme: page.theme; enabled: providerShowId.text.trim().length>0 && Boolean(page.localMetadata) && !page.localMetadata.busy; onClicked: { page.localMetadata.identify(page.show.id,providerShowId.text.trim()); identifyShow.close() } }
            }
        }
    }
}
