import QtQuick
Rectangle {
    id: skeleton
    required property var theme
    color: theme.field
    radius: 7
    opacity: 0.6
    SequentialAnimation on opacity {
        running: skeleton.visible; loops: Animation.Infinite
        NumberAnimation { to: 0.95; duration: 750 }
        NumberAnimation { to: 0.6; duration: 750 }
    }
    Accessible.name: "Loading"
}
