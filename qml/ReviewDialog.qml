import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
Dialog {
    id: review
    required property var theme
    property string showTitle: ""
    property bool preview: false
    property var backend: null
    onRejected: { if(backend) backend.cancelReview() }
    property int selectedEpisodes: 0
    property int selectedSegments: 0
    property int activeSeason: 1
    property var selectedTypes: ({})
    property string endpoint: "IntroDB"
    objectName: "submissionReviewDialog"
    parent: Overlay.overlay; anchors.centerIn: parent
    width: Math.min(520, parent ? parent.width - 40 : 520)
    Overlay.modal: Rectangle { color: "#b8000000" }
    title: "Review Submission"; modal: true; focus: true; padding: 20; topPadding: 8
    background: Rectangle { color: review.theme.card; radius: 14; border.color: review.theme.border }
    header: Text { text: review.title; padding: 20; bottomPadding: 8; color: review.theme.text; font.pixelSize: 18; font.bold: true }
    contentItem: ColumnLayout {
        spacing: 10
        Repeater { model: [{label:"Show",value:review.showTitle},{label:"Season",value:String(review.activeSeason)},{label:"Episodes",value:String(review.selectedEpisodes)},{label:"IMDb",value:review.backend ? review.backend.review.imdb || "—" : "—"},{label:"TMDB",value:review.backend ? review.backend.review.tmdb || "—" : "—"},{label:"Submitting",value:Object.keys(review.selectedTypes).join(" / ")},{label:"Segments",value:String(review.selectedSegments)},{label:"Types",value:Object.keys(review.selectedTypes).map(function(kind){return kind+" "+review.selectedTypes[kind]}).join(" / ")},{label:"Endpoint",value:review.endpoint}]
            RowLayout { required property var modelData; Text { text: modelData.label + ":"; color: review.theme.muted; font.pixelSize: 15 } Text { text: modelData.value; color: review.theme.text; font.pixelSize: 15; font.bold: true; elide: Text.ElideRight } Item { Layout.fillWidth: true } }
        }
        Text { Layout.fillWidth: true; font.pixelSize: 12; color: review.theme.muted; text: review.backend ? review.backend.message : "No submission is ready to send."; visible: text !== "" }
        RowLayout { Layout.alignment: Qt.AlignRight; AppButton { text: "Cancel"; enabled: !review.backend || !review.backend.busy; theme: review.theme; onClicked: { if(review.backend) review.backend.cancelReview(); review.close() } } AppButton { objectName: "confirmSubmission"; text: "Confirm"; enabled: Boolean(review.backend && review.backend.ready); theme: review.theme; onClicked: review.backend.confirm() } }
    }
}
