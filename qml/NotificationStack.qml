import QtQuick
import QtQuick.Controls

Popup {
    id: center
    objectName: "notificationCenter"
    property var theme
    property var backend: null
    property real headerBottom: 88
    property real retainedHeight: 0
    function syncVisibility() {
        if (backend && backend.items.length > 0) { emptyTimer.stop(); if(!visible) open() }
        else if(visible) emptyTimer.restart()
    }
    Component.onCompleted: syncVisibility()
    onBackendChanged: syncVisibility()
    onHeightChanged: if(height > 0) retainedHeight = height
    Connections { target: center.backend; function onChanged() { center.syncVisibility() } }
    Timer { id: emptyTimer; interval: 240; onTriggered: if(!center.backend || center.backend.items.length === 0) center.close() }
    readonly property real edgeMargin: 24
    parent: Overlay.overlay
    popupType: Popup.Item
    z: 110; padding: 0; modal: false; dim: false; focus: false
    closePolicy: Popup.NoAutoClose
    width: Math.min(380, parent ? Math.max(0, parent.width - edgeMargin * 2) : 380)
    x: parent ? Math.max(0, parent.width - width - edgeMargin) : 0
    y: headerBottom + 12
    height: Math.min(backend && backend.items.length > 0 ? cards.contentHeight : retainedHeight, parent ? Math.max(0, parent.height - y - edgeMargin) : 600)
    background: Item {}
    contentItem: ListView {
        id: cards
        objectName: "notificationCards"
        model: center.backend; spacing: 10; clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}
        delegate: AbstractButton {
            id: card
            required property var notification
            property real slideOffset: 0
            objectName: "notification_" + notification.id
            width: cards.width
            height: copy.implicitHeight + 32
            padding: 16; hoverEnabled: true
            Accessible.name: notification.title + ". " + notification.context + ". " + notification.message + ". " + notification.guidance + ". Click to dismiss."
            Accessible.description: notification.persistent ? "Stays until dismissed" : "Closes automatically after five seconds"
            onClicked: center.backend.dismiss(notification.id)
            HoverHandler { cursorShape: Qt.PointingHandCursor }
            background: Rectangle {
                transform: Translate { x: card.slideOffset }
                radius: 12; color: card.hovered ? center.theme.hover : center.theme.field
                border.color: card.activeFocus ? center.theme.link : center.theme.border
            }
            contentItem: Column {
                id: copy; spacing: 8
                transform: Translate { x: card.slideOffset }
                Text {
                    width: parent.width
                    text: card.notification.title; textFormat: Text.PlainText
                    color: center.theme.text; font.pixelSize: 13; font.bold: true; wrapMode: Text.Wrap
                }
                Text {
                    objectName: "notificationContext_" + card.notification.id
                    width: parent.width; visible: text.length > 0
                    text: card.notification.context; textFormat: Text.PlainText
                    color: center.theme.muted; font.pixelSize: 12; font.weight: Font.DemiBold; wrapMode: Text.Wrap
                }
                Text {
                    objectName: "notificationMessage_" + card.notification.id
                    width: parent.width
                    text: card.notification.message; textFormat: Text.PlainText
                    color: center.theme.text; font.pixelSize: 13; wrapMode: Text.Wrap; lineHeight: 1.2
                }
                Text {
                    objectName: "notificationGuidance_" + card.notification.id
                    width: parent.width; visible: text.length > 0
                    text: card.notification.guidance; textFormat: Text.PlainText
                    color: center.theme.muted; font.pixelSize: 12; wrapMode: Text.Wrap; lineHeight: 1.2
                }
                Text {
                    visible: card.notification.persistent
                    text: "Click to dismiss"; color: center.theme.muted; font.pixelSize: 11
                }
            }
        }
        populate: Transition {
            ParallelAnimation {
                NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 180 }
                NumberAnimation { property: "slideOffset"; from: center.width + center.edgeMargin; to: 0; duration: 220; easing.type: Easing.OutCubic }
            }
        }
        add: Transition {
            ParallelAnimation {
                NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 180 }
                NumberAnimation { property: "slideOffset"; from: center.width + center.edgeMargin; to: 0; duration: 220; easing.type: Easing.OutCubic }
            }
        }
        remove: Transition {
            ParallelAnimation {
                NumberAnimation { property: "opacity"; to: 0; duration: 180 }
                NumberAnimation { property: "slideOffset"; to: center.width + center.edgeMargin; duration: 220; easing.type: Easing.InCubic }
            }
        }
        displaced: Transition { NumberAnimation { properties: "y"; duration: 180; easing.type: Easing.OutCubic } }
    }
}
