import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
Column {
    id: extras
    required property var theme
    property string section: "application"
    property bool localProvider: false
    property var mediaLibrary: null
    property var mediaSettings: null
    property var folderDialog: null
    property bool hasTvFolders: true
    property bool hasMovieFolders: true
    property var metadata: mediaLibrary ? mediaLibrary.localMetadata : null
    onVisibleChanged: { if(visible && section === "media" && metadata) metadata.activate() }
    onMetadataChanged: { if(visible && section === "media" && metadata) metadata.activate() }
    spacing: 12
    Column {
        visible: extras.section === "application"; width: parent.width; spacing: 16
        Item {
            width: extras.width; height: 40
            Column { anchors.left: parent.left; spacing: 3; Text { text: "Image cache"; color: extras.theme.text; font.bold: true; font.pixelSize: 15 } Text { text: extras.mediaLibrary ? extras.mediaLibrary.imageCacheSummary : "Unavailable"; color: extras.theme.muted; font.pixelSize: 14 } }
            AppButton { objectName: "clearImageCacheControl"; anchors.right: parent.right; text: "Clear cache"; theme: extras.theme; enabled: Boolean(extras.mediaLibrary) && !extras.mediaLibrary.busy; implicitHeight: 38; onClicked: extras.mediaLibrary.clearImageCache() }
        }
        Rectangle { width: parent.width; height: 1; color: extras.theme.border }
    }
    Text {
        visible: extras.section === "media" && text.length > 0
        text: extras.mediaSettings ? extras.mediaSettings.message : ""
        color: extras.theme.warning; font.pixelSize: 13; width: parent.width; wrapMode: Text.WordWrap
    }
    Repeater {
        model: extras.section === "media" ? ["tv", "movies"] : []
        Column {
            id: mediaSection
            required property string modelData
            readonly property bool movies: modelData === "movies"
            readonly property var configuration: !extras.metadata ? ({})
                : movies ? extras.metadata.movieConfiguration : extras.metadata.configuration
            readonly property var credentials: !extras.metadata ? ({})
                : movies ? extras.metadata.movieCredentials : extras.metadata.credentials
            readonly property bool activeFetch: Boolean(extras.metadata && extras.metadata.fetching && extras.metadata.status.type === modelData)
            readonly property bool hasFolders: movies ? extras.hasMovieFolders : extras.hasTvFolders
            objectName: movies ? "localMovieConfiguration" : "localTvConfiguration"
            width: extras.width; spacing: 12
            Rectangle { visible: mediaSection.movies; width: parent.width; height: 1; color: extras.theme.border }
            Text {
                text: mediaSection.movies ? "Movies" : "TV Shows"
                color: extras.theme.text; font.pixelSize: 15; font.bold: true
            }
            Column {
                width: parent.width; spacing: 8
                Text { text: "Local media folders"; font.pixelSize: 15; font.bold: true; color: extras.theme.text }
                Repeater {
                    model: extras.mediaSettings ? extras.mediaSettings.localFolders.length : 0
                    RowLayout {
                        id: folderRow
                        required property int index
                        readonly property var folder: extras.mediaSettings.localFolders[index]
                        visible: Boolean(folder && folder.type === mediaSection.modelData)
                        width: parent.width; spacing: 8
                        AppTextField {
                            objectName: !folderRow.visible ? "" : folderRow.index === 0 ? "mediaAddressControl" : "localMediaPath" + folderRow.index
                            Accessible.name: "Local media folder " + (folderRow.index + 1)
                            Layout.fillWidth: true; Layout.preferredHeight: 40
                            text: folderRow.folder ? folderRow.folder.path : ""
                            placeholderText: folderRow.folder && folderRow.folder.type === "movies" ? "Movies folder" : "TV shows folder"
                            onTextEdited: extras.mediaSettings.setLocalFolderPath(folderRow.index, text)
                        }
                        AppButton {
                            objectName: !folderRow.visible ? "" : folderRow.index === 0 ? "localBrowseControl" : "localBrowseControl" + folderRow.index
                            text: "Browse"; theme: extras.theme; Layout.preferredWidth: 74; Layout.preferredHeight: 40
                            onClicked: {
                                extras.folderDialog.folderIndex = folderRow.index
                                extras.folderDialog.open()
                            }
                        }
                        AppComboBox {
                            objectName: folderRow.visible ? "localMediaType" + folderRow.index : ""
                            Accessible.name: "Media type for folder " + (folderRow.index + 1)
                            Layout.preferredWidth: 110; implicitHeight: 40
                            model: ["TV Shows", "Movies"]
                            currentIndex: folderRow.folder && folderRow.folder.type === "movies" ? 1 : 0
                            onActivated: function(index) { extras.mediaSettings.setLocalFolderType(folderRow.index, index === 1 ? "movies" : "tv") }
                        }
                        AppButton {
                            objectName: folderRow.visible ? "removeLocalFolder" + folderRow.index : ""
                            text: "×"; hint: "Remove folder"; Accessible.name: hint; theme: extras.theme
                            Layout.preferredWidth: 36; Layout.preferredHeight: 40
                            enabled: folderRow.folder && (folderRow.folder.path.length > 0 || extras.mediaSettings.localFolders.length > 1)
                            onClicked: extras.mediaSettings.removeLocalFolder(folderRow.index)
                        }
                    }
                }
                AppButton {
                    objectName: mediaSection.movies ? "addLocalMovieFolderControl" : "addLocalFolderControl"
                    text: "Add folder"; theme: extras.theme
                    onClicked: extras.mediaSettings.addLocalFolderForType(mediaSection.modelData)
                }
            }
            RowLayout {
                width: parent.width; spacing: 12
                ColumnLayout {
                    Layout.fillWidth: true; spacing: 6
                    Text { text: "Metadata provider"; color: extras.theme.text; font.pixelSize: 15; font.bold: true }
                    AppComboBox {
                        objectName: mediaSection.movies ? "localMovieMetadataProviderControl" : "localMetadataProviderControl"
                        Layout.fillWidth: true
                        readonly property var ids: mediaSection.movies ? ["tmdb","tvdb","omdb"] : ["tmdb","tvdb","omdb","tvmaze"]
                        model: mediaSection.movies ? ["TMDB","TheTVDB","OMDb"] : ["TMDB","TheTVDB","OMDb","TVmaze"]
                        enabled: Boolean(extras.metadata) && !extras.metadata.fetching
                        currentIndex: Math.max(0, ids.indexOf(mediaSection.configuration.provider))
                        onActivated: function(index) {
                            if (mediaSection.movies) extras.metadata.setMovieOption("provider", ids[index])
                            else extras.metadata.setOption("provider", ids[index])
                        }
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true; spacing: 6
                    Text { text: "Artwork provider"; color: extras.theme.text; font.pixelSize: 15; font.bold: true }
                    AppComboBox {
                        objectName: mediaSection.movies ? "localMovieArtworkProviderControl" : "localArtworkProviderControl"
                        Layout.fillWidth: true
                        readonly property var ids: mediaSection.movies ? ["same","tmdb","tvdb","omdb","fanart","off"]
                            : ["same","tmdb","tvdb","omdb","tvmaze","fanart","off"]
                        model: mediaSection.movies ? ["Same as metadata","TMDB","TheTVDB","OMDb","fanart.tv","Off"]
                            : ["Same as metadata","TMDB","TheTVDB","OMDb","TVmaze","fanart.tv","Off"]
                        enabled: Boolean(extras.metadata) && !extras.metadata.fetching
                        currentIndex: Math.max(0, ids.indexOf(mediaSection.configuration.artwork))
                        onActivated: function(index) {
                            if (mediaSection.movies) extras.metadata.setMovieOption("artwork", ids[index])
                            else extras.metadata.setOption("artwork", ids[index])
                        }
                    }
                }
            }
            Repeater {
                model: {
                    const result = []
                    const source = mediaSection.configuration.provider
                    const artwork = mediaSection.configuration.artwork
                    if (source && source !== "tvmaze") result.push(source)
                    if (artwork && artwork !== "same" && artwork !== "off" && artwork !== "tvmaze" && result.indexOf(artwork) < 0)
                        result.push(artwork)
                    return result
                }
                FormRow {
                    required property string modelData
                    width: parent.width; theme: extras.theme; kind: "secret"
                    fieldKey: (mediaSection.movies ? "movie_" : "") + modelData + "_api_key"
                    label: ({tmdb:"TMDB API Key",tvdb:"TheTVDB application API key",omdb:"OMDb API key",fanart:"fanart.tv personal API key"})[modelData]
                    value: String(mediaSection.credentials[modelData] || "")
                    onValueEdited: function(value) {
                        if (mediaSection.movies) extras.metadata.queueMovieCredential(modelData, value)
                        else extras.metadata.queueCredential(modelData, value)
                    }
                }
            }
            RowLayout {
                visible: !mediaSection.movies && mediaSection.configuration.provider === "tvdb"
                width: parent.width
                Text { text: "Episode order"; color: extras.theme.text; font.pixelSize: 14; Layout.fillWidth: true }
                AppComboBox {
                    objectName: mediaSection.movies ? "" : "localEpisodeOrderControl"
                    property var ids: ["official","default","dvd","alternate","absolute","regional"]
                    model: ["Aired / official","TVDB default","DVD","Alternate","Absolute","Regional"]
                    enabled: Boolean(extras.metadata) && !extras.metadata.fetching
                    currentIndex: Math.max(0, ids.indexOf(mediaSection.configuration.episode_order))
                    onActivated: function(index) { extras.metadata.setOption("episode_order", ids[index]) }
                }
            }
            RowLayout {
                visible: mediaSection.movies
                width: parent.width; height: 34
                Text { Layout.fillWidth: true; text: "Import movie timestamps"; color: extras.theme.text }
                SegmentSwitch {
                    objectName: mediaSection.movies && extras.visible ? "mediaImportMovieSegmentsControl" : ""
                    Accessible.name: "Import movie timestamps"
                    checked: extras.mediaSettings ? extras.mediaSettings.importMovieSegments : false
                    onToggled: if (extras.mediaSettings) extras.mediaSettings.setImportMovieSegments(checked)
                }
            }
            Text {
                visible: Boolean(extras.metadata && extras.metadata.status.type === mediaSection.modelData && text.length > 0)
                text: extras.metadata ? extras.metadata.message : ""
                width: parent.width; wrapMode: Text.WordWrap; font.pixelSize: 13; color: extras.theme.muted
            }
            Row {
                width: parent.width; spacing: 8
                AppButton {
                    objectName: !extras.visible ? "" : mediaSection.movies ? "rebuildLocalMovieCacheControl" : "rebuildMediaCacheControl"
                    readonly property bool rebuilding: Boolean(extras.mediaLibrary && extras.mediaLibrary.rebuildingCache
                        && extras.mediaLibrary.localRebuildType === mediaSection.modelData)
                    text: rebuilding ? "Checking for changes…" : "Rescan for changes"
                    theme: extras.theme
                    enabled: mediaSection.hasFolders && Boolean(extras.mediaLibrary) && !extras.mediaLibrary.busy
                        && !extras.mediaLibrary.rebuildingCache && Boolean(extras.metadata) && !extras.metadata.busy
                    progress: rebuilding ? extras.mediaLibrary.cacheRebuildProgress : -1
                    onClicked: extras.mediaLibrary.rebuildLocalMedia(mediaSection.modelData)
                }
                AppButton {
                    objectName: mediaSection.movies ? "clearLocalMovieCacheControl" : "clearLocalCacheControl"
                    text: "Clear local cache"; theme: extras.theme
                    enabled: mediaSection.hasFolders && Boolean(extras.mediaLibrary) && !extras.mediaLibrary.busy
                        && Boolean(extras.metadata) && !extras.metadata.busy
                    onClicked: localClear.open()
                }
            }
            ActionDialog {
                id: localClear
                objectName: mediaSection.movies ? "clearLocalMovieCacheDialog" : "clearLocalCacheDialog"
                theme: extras.theme
                parent: Overlay.overlay; anchors.centerIn: parent
                title: "Clear local cache?"; confirmText: "Delete"
                message: "This clears cached metadata and artwork for the configured Local " + (mediaSection.movies ? "movie" : "TV") + " folders."
                onConfirmed: extras.mediaLibrary.clearLocalMedia(mediaSection.modelData)
            }
            AppButton {
                objectName: mediaSection.movies ? "fetchLocalMovieMetadataControl" : "fetchLocalMetadataControl"
                text: mediaSection.activeFetch ? "Stop fetch" : "Fetch metadata"
                theme: extras.theme
                enabled: Boolean(extras.metadata) && mediaSection.hasFolders
                    && (!extras.mediaLibrary.busy || mediaSection.activeFetch)
                    && (!extras.metadata.busy || mediaSection.activeFetch)
                progress: mediaSection.activeFetch ? Number(extras.metadata.status.progress || 0) : -1
                onClicked: {
                    if (mediaSection.activeFetch) extras.metadata.stop()
                    else extras.metadata.fetchMedia(mediaSection.modelData)
                }
            }
        }
    }
}
