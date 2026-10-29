import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects

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
    Item {
        objectName: "headerGlass"
        parent: header.glassHost || standardGlassHost
        z: -1
        anchors.fill: parent
        clip: true
        Rectangle { anchors.fill: parent; color: header.theme.background; visible: header.blurredBackdrop }
        ShaderEffectSource {
            id: backdrop
            sourceItem: header.blurredBackdrop ? header.backdropSource : null
            sourceRect: header.backdropRect
            textureSize: Qt.size(parent.width,parent.height)
            live: header.blurredBackdrop; visible: false
        }
        MultiEffect {
            objectName: "headerBackdropBlur"
            anchors.fill: parent
            source: backdrop; visible: header.blurredBackdrop
            blurEnabled: true; blurMax: 40; blur: 1.0
            saturation: 0.1
            autoPaddingEnabled: false
        }
        Rectangle {
            objectName: "headerGlassTint"
            anchors.fill: parent; color: header.theme.background
            opacity: header.blurredBackdrop ? 0.72 : 0.88
        }
        Rectangle {
            // Keep the header highlight identical on every page, even where
            // the backdrop continues farther down behind pinned information.
            anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
            height: header.height+40
            gradient: Gradient {
                GradientStop { position: 0; color: "#12ffffff" }
                GradientStop { position: 1; color: "#00ffffff" }
            }
        }
        MouseArea { anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.ArrowCursor; onWheel: function(wheel) { wheel.accepted=false } }
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
        background: Rectangle { color: header.theme.card; radius: 8; border.width: parent.activeFocus ? 2 : 1; border.color: parent.activeFocus ? header.theme.accent : header.theme.border }
    }
    Row {
        anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; spacing: 8
        Repeater {
            model: [{page:"home", label:"TV Shows", icon:"▣"}, {page:"queue", label:"Submission Queue", icon:"☷"}, {page:"history", label:"Submission History", icon:"◴"}, {page:"settings", label:"Settings", icon:"⚙"}]
            AppButton {
                required property var modelData
                objectName: "navigate_" + modelData.page
                width: 40; height: 36; implicitWidth: 40
                text: modelData.icon; hint: modelData.label
                fontSize: 18
                iconName: modelData.page
                outlineSelection: true
                selected: header.navigation.page === modelData.page
                theme: header.theme
                onClicked: header.go(modelData.page)
            }
        }
    }
}
