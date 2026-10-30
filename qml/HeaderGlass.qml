import QtQuick
import QtQuick.Effects

Item {
    id: glass
    required property var theme
    property Item backdropSource: null
    property rect backdropRect
    property real highlightHeight: 84
    readonly property bool blurredBackdrop: backdropSource !== null && GraphicsInfo.api !== GraphicsInfo.Software
    clip: true

    Rectangle { anchors.fill: parent; color: glass.theme.background; visible: glass.blurredBackdrop }
    ShaderEffectSource {
        id: backdrop
        sourceItem: glass.blurredBackdrop ? glass.backdropSource : null
        sourceRect: glass.backdropRect
        textureSize: Qt.size(parent.width,parent.height)
        live: glass.blurredBackdrop; visible: false
    }
    MultiEffect {
        objectName: "headerBackdropBlur"
        anchors.fill: parent
        source: backdrop; visible: glass.blurredBackdrop
        blurEnabled: true; blurMax: 40; blur: 1.0
        saturation: 0.1
        autoPaddingEnabled: false
    }
    Rectangle {
        objectName: "headerGlassTint"
        anchors.fill: parent; color: glass.theme.background
        opacity: glass.blurredBackdrop ? 0.72 : 0.88
    }
    Rectangle {
        anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
        height: glass.highlightHeight
        gradient: Gradient {
            GradientStop { position: 0; color: "#12ffffff" }
            GradientStop { position: 1; color: "#00ffffff" }
        }
    }
    MouseArea { anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.ArrowCursor; onWheel: function(wheel) { wheel.accepted=false } }
}
