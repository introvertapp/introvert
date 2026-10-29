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
    FormRow { id: tmdbKey; enabled: Boolean(extras.metadata); visible: extras.section === "media"; width: parent.width; theme: extras.theme; label: "TMDB API Key"; fieldKey: extras.section === "media" ? "tmdb_api_key" : "unused_tmdb_api_key"; kind: "secret"; secretSaved: Boolean(extras.metadata && extras.metadata.hasKey); onValueEdited: function(value) { if(extras.metadata) extras.metadata.queueKey(value) } }
    Flow {
        width: parent.width; spacing: 8
        AppButton { objectName: extras.section === "media" ? "fetchLocalMetadataControl" : ""; enabled: Boolean(extras.metadata) && (!extras.mediaLibrary.busy || extras.metadata.fetching) && (!extras.metadata.busy || extras.metadata.fetching); visible: extras.section === "media"; text: extras.metadata && extras.metadata.fetching ? "Stop fetch" : "Fetch metadata"; theme: extras.theme; onClicked: { if(extras.metadata.fetching) extras.metadata.stop(); else extras.metadata.fetch() } }
        AppButton { objectName: extras.section === "media" ? "clearLocalCacheControl" : ""; visible: extras.section === "media"; text: "Clear local cache"; theme: extras.theme; enabled: Boolean(extras.mediaLibrary) && !extras.mediaLibrary.busy && Boolean(extras.metadata) && !extras.metadata.busy; onClicked: clearLocal.open() }
    }
    ActionDialog {
        id: clearLocal; objectName: extras.section === "media" ? "clearLocalCacheDialog" : ""; theme: extras.theme
        title: "Clear local cache?"; confirmText: "Delete"
        message: "This will delete Local provider metadata for the selected local media path. Your media files will not be deleted."
        onConfirmed: { if(extras.mediaLibrary) extras.mediaLibrary.clearLocalCache() }
    }
}
