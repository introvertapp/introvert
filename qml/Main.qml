import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: window
    required property var appSettings
    required property var appNavigation
    property bool uiPreview: false
    property var mediaLibrary: null
    onMediaLibraryChanged: if(mediaLibrary) mediaLibrary.setBrowsing(appNavigation.page === "home")
    property var videoEditor: null
    property var segmentManager: null
    property var submissions: null
    property var detector: null
    property var applicationUpdates: null
    property var selectedShow: null
    property bool openingShowRequest: false
    property string searchQuery: ""
    property string visualState: "ready"
    // Presentation inputs for isolated UI preview and model tests. Empty in normal startup.
    property var libraryItems: []
    property var queueItems: []
    property var historyItems: []
    property var episodeItems: []
    readonly property var presentation: previewLoader.item
    objectName: "introvertWindow"
    visible: true
    width: 1200; height: 810; minimumWidth: 870; minimumHeight: 570
    title: uiPreview ? "Introvert — UI preview" : "Introvert"
    color: colors.background
    font.pixelSize: 14
    font.weight: Font.Medium
    font.family: Qt.platform.os === "osx" ? "Helvetica Neue" : "sans-serif"
    palette.window: colors.background
    palette.windowText: colors.text
    palette.text: colors.text
    palette.base: colors.field
    palette.button: colors.card
    palette.buttonText: colors.text
    palette.highlight: colors.accent
    palette.highlightedText: colors.text
    palette.mid: colors.border
    palette.placeholderText: colors.muted
    Shortcut { sequence: StandardKey.Quit; onActivated: Qt.quit() }
    menuBar: MenuBar {
        visible: Qt.platform.os === "osx"
        Menu { title: "Window"; Action { text: "Show Introvert"; onTriggered: {window.show();window.raise();window.requestActivate()} } }
    }
    UiTheme { id: colors; objectName: "applicationTheme" }
    Loader { id: previewLoader; active: window.uiPreview; source: "PreviewData.qml" }
    function explain(title, message) { action.title = title; action.message = message; action.confirmText = "Close"; action.open() }
    function openShow(show) { appNavigation.navigate("home"); selectedShow = show; if(mediaLibrary && !uiPreview) { openingShowRequest=true; try { mediaLibrary.openShow(show.id) } finally { openingShowRequest=false } } }
    function openEditor() { editor.open() }
    Connections {
        target: window.appNavigation
        function onPageChanged() { if(window.mediaLibrary) window.mediaLibrary.setBrowsing(window.appNavigation.page === "home"); window.selectedShow = null; window.searchQuery = ""; if(window.mediaLibrary && !window.uiPreview) window.mediaLibrary.closeShow() }
    }
    Connections {
        target: window.mediaLibrary || null
        function onCacheRebuildFailed(message) {window.explain("Cache rebuild failed",message)}
        function onChanged() { if(!window.uiPreview && !window.openingShowRequest && window.mediaLibrary.busy && !window.mediaLibrary.show.id) window.selectedShow=null }
    }
    ColumnLayout {
        anchors.fill: parent; anchors.margins: 20; spacing: 20
        AppHeader {
            id: appHeader
            z: 10
            Layout.fillWidth: true
            glassHost: pages.glassPage ? pages.glassPage.glassHost : null
            backdropSource: pages.glassPage ? pages.glassPage.scrollSurface : pages
            backdropRect: pages.glassPage
                ? Qt.rect(-20,0,window.width,pages.glassPage.glassHost.height)
                : Qt.rect(-20,-(pages.y+20),window.width,pages.y+20)
            theme: colors; navigation: window.appNavigation; query: window.searchQuery
            showSearch: window.selectedShow === null
            onNavigationRequested: { window.selectedShow = null; if(window.mediaLibrary && !window.uiPreview) window.mediaLibrary.closeShow() }
            onSearchChanged: function(text) { window.searchQuery = text }
        }
        StackLayout {
            id: pages
            objectName: "mainPageStack"
            readonly property Item glassPage: currentIndex === 0 ? library : currentIndex === 4 ? showDetails : null
            Layout.fillWidth: true; Layout.fillHeight: true
            currentIndex: window.selectedShow ? 4 : window.appNavigation.page === "home" ? 0 : window.appNavigation.page === "queue" ? 1 : window.appNavigation.page === "history" ? 2 : 3
            LibraryPage {
                id: library
                headerInset: pages.y+20
                theme: colors; appSettings: window.appSettings; appNavigation: window.appNavigation
                preview: window.uiPreview; items: window.presentation ? window.presentation.shows : window.mediaLibrary ? window.mediaLibrary.shows : window.libraryItems
                query: window.searchQuery; visualState: window.mediaLibrary && !window.uiPreview ? window.mediaLibrary.state : window.visualState
                mediaLibrary: window.mediaLibrary
                onShowOpened: function(show) { window.openShow(show) }
            }
            QueuePage {
                theme: colors; preview: window.uiPreview; query: window.searchQuery; visualState: window.visualState
                items: window.presentation ? window.presentation.queue : window.detector ? (window.submissions ? window.submissions.queue : window.queueItems).concat(window.detector.queue) : window.submissions ? window.submissions.queue : window.queueItems
                onShowOpened: function(record) { if(window.presentation) window.openShow(window.presentation.shows[0]); else if(record && record.showId) { window.appSettings.setMediaProvider(record.providerId); window.openShow({id:record.showId,title:record.show}) } }
            }
            HistoryPage {
                statistics: window.submissions ? window.submissions.statistics : ({})
                theme: colors; preview: window.uiPreview; query: window.searchQuery; visualState: window.visualState
                items: window.presentation ? window.presentation.history : window.submissions ? window.submissions.history : window.historyItems
                onShowOpened: function(record) { if(window.presentation) window.openShow(window.presentation.shows[0]); else if(record && record.showId) { window.appSettings.setMediaProvider(record.providerId); window.openShow({id:record.showId,title:record.show}) } }
            }
            SettingsPage { applicationUpdates: window.applicationUpdates; segmentManager: window.uiPreview ? null : window.segmentManager; theme: colors; appSettings: window.appSettings; mediaLibrary: window.uiPreview ? null : window.mediaLibrary; preview: window.uiPreview; previewShows: window.presentation ? window.presentation.shows : [] }
            ShowPage {
                id: showDetails
                headerInset: pages.y+20
                timestampFormat: window.appSettings.timestampFormat
                detector: window.uiPreview ? null : window.detector
                submissions: window.uiPreview ? null : window.submissions
                segmentManager: window.uiPreview ? null : window.segmentManager
                localMetadata: window.uiPreview || !window.mediaLibrary ? null : window.mediaLibrary.localMetadata
                theme: colors; show: window.mediaLibrary && !window.uiPreview && window.mediaLibrary.show.id ? window.mediaLibrary.show : window.selectedShow; preview: window.uiPreview
                loading: Boolean(window.mediaLibrary && !window.uiPreview && window.mediaLibrary.busy)
                errorMessage: window.mediaLibrary && !window.uiPreview ? window.mediaLibrary.message : ""
                season: window.mediaLibrary && !window.uiPreview ? window.mediaLibrary.season : 1
                onSeasonRequested: function(number) { if(window.mediaLibrary && !window.uiPreview) window.mediaLibrary.selectSeason(number) }
                episodes: window.presentation ? window.presentation.episodes : window.mediaLibrary ? window.mediaLibrary.episodes : window.episodeItems
                onBackRequested: { window.selectedShow = null; if(window.mediaLibrary && !window.uiPreview) window.mediaLibrary.closeShow() }
                onEditorRequested: function(episodeTitle, episode) { editor.episodeTitle=episodeTitle; editor.loadEpisode(episode); if(window.videoEditor && !window.uiPreview) window.videoEditor.openEpisode(episode.id); editor.open() }
                onReviewRequested: { if(window.submissions && !window.uiPreview) window.submissions.prepare(showDetails.selectedPayload(),showDetails.endpoint); else review.open() }
                onSubmissionErrorRequested: function(title,message) {window.explain(title,message)}
                onUnavailableRequested: function(title) { window.explain(title, "This interface is ready for integration. No operation has been started or saved.") }
            }
        }
    }
    Connections {
        target: window.submissions || null
        function onPrepared() { review.open() }
        function onEnqueued() { review.close(); showDetails.clearSelection() }
    }
    ActionDialog { id: action; theme: colors }
    ReviewDialog { id: review; backend: window.uiPreview ? null : window.submissions; theme: colors; showTitle: window.selectedShow ? window.selectedShow.title : "No show selected"; preview: window.uiPreview; selectedEpisodes: showDetails.selectedCount; selectedSegments: showDetails.selectedSegmentCount; selectedTypes: showDetails.selectedTypes; activeSeason: showDetails.season; endpoint: showDetails.endpoint }
    EditorDialog { id: editor; previewEpisodes: window.presentation ? window.presentation.episodes : window.episodeItems; previewSeasonNumbers: [showDetails.season]; backend: window.videoEditor; theme: colors; timestampFormat: window.appSettings.timestampFormat; preview: window.uiPreview; showTitle: window.selectedShow ? window.selectedShow.title : "Video segment editor" }
}
