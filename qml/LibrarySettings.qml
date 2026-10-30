import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
Column {
    id: extras
    required property var theme
    property string section: "application"
    property bool localProvider: false
    property var mediaLibrary: null
    property var metadata: mediaLibrary ? mediaLibrary.localMetadata : null
    onVisibleChanged: { if(visible && section === "media" && metadata) metadata.activate() }
    onMetadataChanged: { if(visible && section === "media" && metadata) metadata.activate() }
    spacing: 12
    Column {
        visible: extras.section === "application"; width: parent.width; spacing: 16
        Repeater {
            model: ["Image cache", "Media preview cache"]
            Item {
                required property string modelData
                width: extras.width; height: 40
                Column { anchors.left: parent.left; spacing: 3; Text { text: modelData; color: extras.theme.text; font.bold: true; font.pixelSize: 15 } Text { objectName: extras.section === "application" && modelData === "Media preview cache" ? "mediaPreviewCacheSummary" : ""; text: extras.mediaLibrary ? (modelData === "Image cache" ? extras.mediaLibrary.imageCacheSummary : "Not used (direct playback)") : "Unavailable"; color: extras.theme.muted; font.pixelSize: 14 } }
                AppButton { objectName: modelData === "Image cache" ? "clearImageCacheControl" : extras.section === "application" ? "clearMediaPreviewCacheControl" : ""; anchors.right: parent.right; text: "Clear cache"; theme: extras.theme; hint: modelData === "Media preview cache" && extras.mediaLibrary ? "Direct playback does not create cached preview clips" : ""; enabled: modelData === "Image cache" && Boolean(extras.mediaLibrary) && !extras.mediaLibrary.busy; implicitHeight: 38; onClicked: extras.mediaLibrary.clearImageCache() }
            }
        }
        Rectangle { width: parent.width; height: 1; color: extras.theme.border }
    }
    Text { visible: extras.section === "media" && text !== ""; text: extras.metadata ? extras.metadata.message + (extras.metadata.status.total ? " ("+extras.metadata.status.current+" / "+extras.metadata.status.total+" shows)" : "") : ""; color: extras.theme.muted; font.pixelSize: 13; width: parent.width; wrapMode: Text.WordWrap }
    Column {
        visible: extras.section === "media"; width: parent.width; spacing: 10
        RowLayout {
            width: parent.width; spacing: 12
            ColumnLayout {
                Layout.fillWidth: true; spacing: 6
                Text { text: "Metadata provider"; color: extras.theme.text; font.pixelSize: 15; font.bold: true }
                AppComboBox {
                    objectName: "localMetadataProviderControl"; Layout.fillWidth: true
                    property var ids: ["tmdb","tvdb","omdb","tvmaze"]
                    model: ["TMDB","TheTVDB","OMDb","TVmaze"]
                    enabled: Boolean(extras.metadata) && !extras.metadata.fetching
                    currentIndex: extras.metadata ? Math.max(0,ids.indexOf(extras.metadata.configuration.provider)) : 0
                    onActivated: function(index) { extras.metadata.setOption("provider",ids[index]) }
                }
            }
            ColumnLayout {
                Layout.fillWidth: true; spacing: 6
                Text { text: "Artwork provider"; color: extras.theme.text; font.pixelSize: 15; font.bold: true }
                AppComboBox {
                    objectName: "localArtworkProviderControl"; Layout.fillWidth: true
                    property var ids: ["same","tmdb","tvdb","omdb","tvmaze","fanart","off"]
                    model: ["Same as metadata","TMDB","TheTVDB","OMDb","TVmaze","fanart.tv","Off"]
                    enabled: Boolean(extras.metadata) && !extras.metadata.fetching
                    currentIndex: extras.metadata ? Math.max(0,ids.indexOf(extras.metadata.configuration.artwork)) : 0
                    onActivated: function(index) { extras.metadata.setOption("artwork",ids[index]) }
                }
            }
        }
        Repeater {
            model: {
                if(!extras.metadata) return []
                var result=[], source=extras.metadata.configuration.provider
                var artwork=extras.metadata.configuration.artwork
                if(source && source!=="tvmaze") result.push(source)
                if(artwork && artwork!=="same" && artwork!=="off" && artwork!=="tvmaze" && result.indexOf(artwork)<0) result.push(artwork)
                if(result.indexOf("tvdb")>=0) result.splice(result.indexOf("tvdb")+1,0,"tvdb_pin")
                return result
            }
            FormRow {
                required property string modelData
                width: parent.width; theme: extras.theme; kind: "secret"
                fieldKey: modelData+"_api_key"
                label: modelData==="tvdb_pin" ? "TheTVDB subscriber PIN (if required)" : ({tmdb:"TMDB API Key",tvdb:"TheTVDB application API key",omdb:"OMDb API key",fanart:"fanart.tv personal API key"})[modelData]
                value: extras.metadata ? String(extras.metadata.credentials[modelData] || "") : ""
                onValueEdited: function(value) { extras.metadata.queueCredential(modelData,value) }
            }
        }
        RowLayout {
            id: episodeOrderRow
            visible: Boolean(extras.metadata) && extras.metadata.configuration.provider==="tvdb"
            width: parent.width
            Text { text: "Episode order"; color: extras.theme.text; font.pixelSize: 14; Layout.fillWidth: true }
            AppComboBox {
                objectName: "localEpisodeOrderControl"; property var ids: ["official","default","dvd","alternate","absolute","regional"]
                model: ["Aired / official","TVDB default","DVD","Alternate","Absolute","Regional"]
                enabled: Boolean(extras.metadata) && !extras.metadata.fetching && extras.metadata.configuration.provider==="tvdb"
                currentIndex: extras.metadata ? Math.max(0,ids.indexOf(extras.metadata.configuration.episode_order)) : 0
                onActivated: function(index) { extras.metadata.setOption("episode_order",ids[index]) }
            }
        }
        Text { visible: episodeOrderRow.visible; width: parent.width; wrapMode: Text.WordWrap; color: extras.theme.muted; font.pixelSize: 12; text: "Match the numbering used by your files." }
    }
    Flow {
        width: parent.width; spacing: 8
        AppButton { objectName: extras.section === "media" ? "fetchLocalMetadataControl" : ""; enabled: Boolean(extras.metadata) && (!extras.mediaLibrary.busy || extras.metadata.fetching) && (!extras.metadata.busy || extras.metadata.fetching); visible: extras.section === "media"; progress: extras.metadata && extras.metadata.fetching ? Number(extras.metadata.status.current || 0)/Math.max(1,Number(extras.metadata.status.total || 0)) : -1; text: extras.metadata && extras.metadata.fetching ? "Stop fetch" : "Fetch metadata"; theme: extras.theme; onClicked: { if(extras.metadata.fetching) extras.metadata.stop(); else extras.metadata.fetch() } }
        AppButton { objectName: extras.section === "media" ? "clearLocalCacheControl" : ""; visible: extras.section === "media"; text: "Clear local cache"; theme: extras.theme; enabled: Boolean(extras.mediaLibrary) && !extras.mediaLibrary.busy && Boolean(extras.metadata) && !extras.metadata.busy; onClicked: clearLocal.open() }
    }
    ActionDialog {
        id: clearLocal; objectName: extras.section === "media" ? "clearLocalCacheDialog" : ""; theme: extras.theme
        title: "Clear local cache?"; confirmText: "Delete"
        message: "This will delete Local provider metadata for the selected local media path. Your media files will not be deleted."
        onConfirmed: { if(extras.mediaLibrary) extras.mediaLibrary.clearLocalCache() }
    }
}
