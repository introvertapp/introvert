import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: header
    required property var theme
    required property var navigation
    property string query: ""
    property bool showSearch: true
    property Item backdropSource: null
    property Item glassHost: null
    property rect backdropRect
    readonly property bool blurredBackdrop: backdropSource !== null && GraphicsInfo.api !== GraphicsInfo.Software
    signal navigationRequested(string page)
    function go(page) { navigationRequested(page); navigation.navigate(page) }
    signal searchChanged(string text)
    implicitHeight: 44
    objectName: "applicationHeader"
    // Every screen uses the same full-width material host. Scrolling library
    // and detail pages supply a taller host, without changing the material.
    Item {
        id: standardGlassHost
        objectName: "standardHeaderGlassHost"
        z: -1
        anchors.fill: parent; anchors.margins: -20
    }
    HeaderGlass {
        objectName: "headerGlass"
        parent: header.glassHost || standardGlassHost
        z: -1
        anchors.fill: parent
        theme: header.theme; backdropSource: header.backdropSource; backdropRect: header.backdropRect
        // Keep the highlight identical even on taller page material hosts.
        highlightHeight: header.height+40
    }
    AppImage {
        objectName: "applicationWordmark"
        width: 156; height: 44
        source: "assets/wordmark.png"
        fillMode: Image.PreserveAspectFit
        MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: header.go("home") }
    }
    AppTextField {
        objectName: "globalSearchControl"
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.verticalCenter: parent.verticalCenter
        width: parent.width * 0.5; height: 40
        visible: header.showSearch && header.navigation.page !== "settings"
        text: header.query
        placeholderText: header.navigation.page === "queue" ? "Search queue" : header.navigation.page === "history" ? "Search history" : "Search shows or episodes"
        font.pixelSize: 15; color: header.theme.text
        placeholderTextColor: header.theme.muted
        Accessible.name: placeholderText
        onTextEdited: header.searchChanged(text)
        background: Rectangle { color: header.theme.card; radius: 8; border.width: 1; border.color: header.theme.border }
    }
    Row {
        anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; spacing: 8
        Repeater {
            model: [{page:"home", label:"TV Shows", icon:"▣"}, {page:"queue", label:"Submission Queue", icon:"☷"}, {page:"history", label:"Submission History", icon:"◴"}, {page:"settings", label:"Settings", icon:"⚙"}]
            AppButton {
                id: navigationButton
                required property var modelData
                objectName: "navigate_" + modelData.page
                width: 40; height: 36; implicitWidth: 40
                text: modelData.icon; hint: modelData.label
                fontSize: 18
                iconName: modelData.page
                selected: header.navigation.page === modelData.page
                theme: header.theme
                background: Rectangle {
                    radius: 8
                    color: navigationButton.selected ? header.theme.accent : navigationButton.hovered ? header.theme.hover : header.theme.card
                    border.width: 1; border.color: header.theme.border
                }
                onClicked: header.go(modelData.page)
            }
        }
    }
}
