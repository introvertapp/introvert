import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: window
    required property var appSettings
    required property var appNavigation
    property bool uiPreview: false
    property var mediaLibrary: null
    onMediaLibraryChanged: if(mediaLibrary) mediaLibrary.setBrowsePage(appNavigation.page)
    property var videoEditor: null
    property var segmentManager: null
    property var submissions: null
    property var detector: null
    property var applicationUpdates: null
    property var selectedShow: null
    property var selectedMovie: null
    property bool openingShowRequest: false
    property var searchQueries: ({home:"", movies:"", queue:"", history:""})
    readonly property string searchQuery: searchQueries[appNavigation.page] || ""
    function updateSearch(text) {
        var queries = Object.assign({}, searchQueries)
        queries[appNavigation.page] = text
        searchQueries = queries
    }
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
    title: uiPreview ? "INTROvert — UI preview" : "INTROvert"
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
    Shortcut { objectName: "quitShortcut"; sequences: [StandardKey.Quit]; onActivated: Qt.quit() }
    menuBar: MenuBar {
        visible: Qt.platform.os === "osx"
        Menu { title: "Window"; Action { text: "Show INTROvert"; onTriggered: {window.show();window.raise();window.requestActivate()} } }
    }
    UiTheme { id: colors; objectName: "applicationTheme" }
    Loader { id: previewLoader; active: window.uiPreview; source: "PreviewData.qml" }
    property var applicationLogs: null
    property var appNotifications: null
    function explain(title, message) { action.title = title; action.message = message; action.confirmText = "Close"; action.open() }
    function openShow(show) {
        appNavigation.navigate("home")
        if(mediaLibrary && appSettings.mediaProvider !== "local" && appSettings.mediaSettings.importMode === "movies") return
        selectedShow = show
        if(mediaLibrary && !uiPreview) {
            openingShowRequest = true
            try { if(show.scope) mediaLibrary.openJobShow(show.id,show.scope); else mediaLibrary.openShow(show.id) }
            finally { openingShowRequest = false }
        }
    }
    function openMovie(movie) {
        appNavigation.navigate("movies")
        if(mediaLibrary && appSettings.mediaProvider !== "local" && appSettings.mediaSettings.importMode === "tv") return
        selectedMovie = movie
        if(mediaLibrary && !uiPreview) {
            if(movie.scope) mediaLibrary.openJobMovie(movie.id, movie.scope)
            else mediaLibrary.openMovie(movie.id)
        }
    }
    function openJob(record) {
        if(window.presentation) { window.openShow(window.presentation.shows[0]); return }
        if(!record || !record.showId) return
        appSettings.setMediaProvider(record.providerId)
        var title = {id:record.showId, title:record.show, scope:record.scope || ""}
        if(record.movie) window.openMovie(title)
        else window.openShow(title)
    }
    function closeDetail() {
        selectedShow = null
        selectedMovie = null
        if(mediaLibrary && !uiPreview) mediaLibrary.closeShow()
    }
    function openEditor() { editor.open() }
    Connections {
        target: window.detector || null
        function onDetectorRequired(message) { window.explain("Detector unavailable",message) }
    }
    Connections {
        target: window.appNavigation
        function onPageChanged() {
            window.closeDetail()
            if(window.mediaLibrary) window.mediaLibrary.setBrowsePage(window.appNavigation.page)
        }
    }
    Connections {
        target: window.mediaLibrary || null
        function onCacheRebuildFailed(message) {window.explain("Cache rebuild failed",message)}
        function onChanged() {
            if(window.uiPreview) return
            if(!window.openingShowRequest && window.mediaLibrary.busy && !window.mediaLibrary.show.id) window.selectedShow = null
            if(window.selectedMovie && !window.mediaLibrary.movie.id) window.selectedMovie = null
        }
    }
    Connections {
        target: window.submissions || null
        function onQueueOrderChanged() { submissionQueue.refreshOrder(); if(window.detector && !window.uiPreview) window.detector.refreshQueueOrder() }
    }
    ColumnLayout {
        id: shellLayout
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
            homePage: window.appSettings.mediaSettings.importMode === "movies" ? "movies" : "home"
            showSearch: window.selectedShow === null && window.selectedMovie === null
            onNavigationRequested: window.closeDetail()
            onSearchChanged: function(text) { window.updateSearch(text) }
        }
        StackLayout {
            id: pages
            objectName: "mainPageStack"
            readonly property Item glassPage: currentIndex === 0 ? library : currentIndex === 4 ? showDetails : null
            Layout.fillWidth: true; Layout.fillHeight: true
            currentIndex: window.selectedMovie || window.selectedShow ? 4 : window.appNavigation.page === "queue" ? 1 : window.appNavigation.page === "history" ? 2 : window.appNavigation.page === "settings" ? 3 : 0
            LibraryPage {
                id: library
                headerInset: pages.y+20
                theme: colors; appSettings: window.appSettings; appNavigation: window.appNavigation
                movies: window.appNavigation.page === "movies"
                preview: window.uiPreview
                items: window.presentation ? (movies ? window.presentation.movies : window.presentation.shows)
                    : window.mediaLibrary ? (movies ? window.mediaLibrary.movies : window.mediaLibrary.shows)
                    : movies ? [] : window.libraryItems
                query: window.searchQuery; visualState: window.mediaLibrary && !window.uiPreview ? window.mediaLibrary.state : window.visualState
                mediaLibrary: window.mediaLibrary
                detector: window.uiPreview || movies ? null : window.detector
                onShowOpened: function(record) { if(movies) window.openMovie(record); else window.openShow(record) }
            }
            QueuePage {
                id: submissionQueue
                theme: colors; preview: window.uiPreview; query: window.searchQueries.queue; visualState: window.visualState
                items: window.presentation ? window.presentation.queue : window.detector ? (window.submissions ? window.submissions.queue : window.queueItems).concat(window.detector.queue) : window.submissions ? window.submissions.queue : window.queueItems
                queuePositions: window.submissions ? window.submissions.queuePositions : ({})
                onMoveRequested: function(key, target, after) { if(window.submissions && !window.uiPreview) window.submissions.moveQueueItem(key, target, after, submissionQueue.requestedOrder) }
                onShowOpened: function(record) { window.openJob(record) }
            }
            HistoryPage {
                statistics: window.submissions ? window.submissions.statistics : ({})
                theme: colors; preview: window.uiPreview; query: window.searchQueries.history; visualState: window.visualState
                items: window.presentation ? window.presentation.history : window.submissions ? window.submissions.history : window.historyItems
                onShowOpened: function(record) { window.openJob(record) }
            }
            SettingsPage { detector: window.uiPreview ? null : window.detector; appNotifications: window.appNotifications; applicationLogs: window.applicationLogs; applicationUpdates: window.applicationUpdates; segmentManager: window.uiPreview ? null : window.segmentManager; theme: colors; appSettings: window.appSettings; mediaLibrary: window.uiPreview ? null : window.mediaLibrary; preview: window.uiPreview; previewShows: window.presentation ? window.presentation.shows : [] }
            ShowPage {
                id: showDetails
                movieMode: window.selectedMovie !== null
                mediaLibrary: window.uiPreview ? null : window.mediaLibrary
                headerInset: pages.y+20
                timestampFormat: window.appSettings.timestampFormat
                detector: window.uiPreview || movieMode ? null : window.detector
                submissions: window.uiPreview ? null : window.submissions
                segmentManager: window.uiPreview ? null : window.segmentManager
                localMetadata: window.uiPreview || movieMode || !window.mediaLibrary ? null : window.mediaLibrary.localMetadata
                theme: colors; preview: window.uiPreview
                show: {
                    if(movieMode) return window.mediaLibrary && !window.uiPreview && window.mediaLibrary.movie.title ? window.mediaLibrary.movie : window.selectedMovie
                    return window.mediaLibrary && !window.uiPreview && window.mediaLibrary.show.id ? window.mediaLibrary.show : window.selectedShow
                }
                loading: Boolean(window.mediaLibrary && !window.uiPreview && window.mediaLibrary.busy)
                errorMessage: window.mediaLibrary && !window.uiPreview ? window.mediaLibrary.message : ""
                season: window.mediaLibrary && !window.uiPreview ? window.mediaLibrary.season : 1
                onSeasonRequested: function(number) { if(window.mediaLibrary && !window.uiPreview) window.mediaLibrary.selectSeason(number) }
                episodes: movieMode ? (window.mediaLibrary && !window.uiPreview ? window.mediaLibrary.movieItems : show ? [{id:show.id, title:show.title, movie:true}] : [])
                    : window.presentation ? window.presentation.episodes : window.mediaLibrary ? window.mediaLibrary.episodes : window.episodeItems
                onBackRequested: window.closeDetail()
                onEditorRequested: function(episodeTitle, episode) { editor.episodeTitle=episodeTitle; editor.loadEpisode(episode); if(window.videoEditor && !window.uiPreview) window.videoEditor.openEpisode(episode.id); editor.open() }
                onReviewRequested: { if(window.submissions && !window.uiPreview) window.submissions.prepareMany(showDetails.selectedPayload(),showDetails.requestedEndpoints); else review.open() }
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
    NotificationStack { theme: colors; backend: window.appNotifications; headerBottom: window.contentItem.y + shellLayout.y + appHeader.y + appHeader.height }
    ActionDialog { id: action; theme: colors }
    ReviewDialog { id: review; backend: window.uiPreview ? null : window.submissions; theme: colors; showTitle: showDetails.show ? showDetails.show.title : ""; movieMode: showDetails.movieMode; preview: window.uiPreview; selectedEpisodes: showDetails.selectedCount; selectedSegments: showDetails.selectedSegmentCount; selectedTypes: showDetails.selectedTypes; activeSeason: showDetails.season; endpoint: showDetails.endpoint }
    EditorDialog { id: editor; previewEpisodes: window.presentation ? showDetails.episodes : window.episodeItems; previewSeasonNumbers: [showDetails.season]; backend: window.videoEditor; theme: colors; timestampFormat: window.appSettings.timestampFormat; preview: window.uiPreview; movieMode: showDetails.movieMode; showTitle: showDetails.show ? showDetails.show.title : "Video segment editor" }
}
