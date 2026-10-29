import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Window

AbstractButton {
    id: tile
    HoverHandler { cursorShape: Qt.PointingHandCursor }
    required property var theme
    required property var record
    property string matchText: ""
    objectName: "poster_" + record.id
    hoverEnabled: true
    Accessible.name: record.title
    readonly property bool gpuMask: GraphicsInfo.api !== GraphicsInfo.Software
    Rectangle {
        id: posterMask
        width: tile.width; height: tile.height; radius: 14
        color: "white"; antialiasing: true
        visible: false; layer.enabled: tile.gpuMask; layer.smooth: true
    }
    background: Rectangle {
        radius: tile.gpuMask ? 0 : 14
        color: tile.record.color || "#263744"; clip: true
        // Mask the composited artwork, dimming and labels together. Covering
        // corners with background-colored paint leaves edge fringes on the GPU.
        layer.enabled: tile.gpuMask
        layer.effect: MultiEffect {
            maskEnabled: true; maskSource: posterMask
            // Preserve the mask's fractional edge coverage; the default sharp
            // threshold discards antialiasing and produces stair-stepped curves.
            maskThresholdMin: 0.5
            maskSpreadAtMin: 1.0
            autoPaddingEnabled: false
        }
        Item {
            objectName: "posterArtwork"
            anchors.fill: parent
            scale: tile.hovered ? 1.025 : 1
            Behavior on scale { NumberAnimation { duration: 350 } }
        AppImage {
            id: posterImage; objectName: "posterImage"
            anchors.fill: parent; source: tile.record.posterUrl || ""; fillMode: Image.PreserveAspectCrop
            // Posters retain their full physical decode even when artwork is SVG.
            sourceSize.width: Math.ceil(160 * Screen.devicePixelRatio * 2)
            sourceSize.height: Math.ceil(240 * Screen.devicePixelRatio * 2)
            // Sample the 2x decode directly throughout the small hover zoom.
            // Minification mip levels discard detail that zooming then enlarges.
            mipmap: false
        }
        // Original geometric poster compositions, limited to design preview data.
        Rectangle { visible: !tile.record.posterUrl; width: parent.width * 1.1; height: width; radius: width / 2; x: parent.width * 0.22; y: parent.height * 0.1; color: "#18ffffff"; rotation: 25 }
        Rectangle { visible: !tile.record.posterUrl; width: parent.width * 0.5; height: parent.height; rotation: 32; x: -parent.width * 0.15; color: "#16000000" }
        Text { visible: !tile.record.posterUrl; anchors.horizontalCenter: parent.horizontalCenter; y: parent.height * 0.35; width: parent.width * 0.85; text: tile.record.title.toUpperCase(); wrapMode: Text.WordWrap; horizontalAlignment: Text.AlignHCenter; font.pixelSize: Math.max(16, tile.width * 0.12); font.letterSpacing: 2; font.bold: true; color: "#bfffffff" }
        }
        Rectangle { objectName: "posterHoverDim"; anchors.fill: parent; color: "black"; opacity: tile.hovered ? .12 : 0; Behavior on opacity { NumberAnimation { duration: 180 } } }
        Rectangle { objectName: "posterGradient"; anchors.fill: parent; gradient: Gradient { GradientStop { position: 0.45; color: "transparent" } GradientStop { position: 1; color: "#ee000000" } } }
        Column { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: 10; spacing: 5
            Text { objectName: "posterOverlayTitle"; width: parent.width; text: tile.record.title; color: "white"; wrapMode: Text.WordWrap; font.pixelSize: 15; font.bold: true }
            Text { objectName: "posterOverlayDetails"; opacity: tile.hovered || tile.activeFocus ? 1 : 0; Behavior on opacity { NumberAnimation { duration: 180 } } width: parent.width; text: tile.matchText || tile.record.seasons + " seasons · " + tile.record.episodes + " episodes"; color: tile.matchText ? "#8ab4ff" : "#b3ffffff"; font.pixelSize: 12; elide: Text.ElideRight }
        }
        RoundedFrame { objectName: "posterCornerFrame"; coverCorners: !tile.gpuMask; edgeSoftness: 2; anchors.fill: parent; radius: 14; outsideColor: tile.theme.background; borderWidth: 0 }
        Rectangle { anchors.fill: parent; radius: 14; color: "transparent"; border.width: tile.visualFocus ? 1 : 0; border.color: tile.theme.muted }
    }
}
