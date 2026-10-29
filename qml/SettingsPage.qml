import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

Item {
    id: page
    property var theme
    property var mediaLibrary: null
    property var applicationUpdates: null
    required property var appSettings
    readonly property real navigationReveal: settingsSplit.panelReveal
    function toggleNavigationWidth() { settingsSplit.togglePanelWidth() }
    property string activeTab: "application"
    onActiveTabChanged: { if(activeTab === "endpoints" && appSettings && appSettings.endpointSettings) appSettings.endpointSettings.activate(); if(activeTab === "notifications" && appSettings && appSettings.webhookSettings) appSettings.webhookSettings.activate() }
    property bool preview: false
    property var segmentManager: null
    property var previewShows: []
    FolderDialog { id: localFolder; objectName: "localBrowseDialog"; title: "Local media path"; onAccepted: page.appSettings.mediaSettings.setDirectoryUrl(selectedFolder.toString()) }

    Rectangle {
        objectName: "settingsSurface"
        anchors.fill: parent
        color: page.theme.card; border.color: page.theme.border; radius: 14
        clip: true

        CollapsibleSplitView {
            id: settingsSplit
            objectName: "settingsSplitView"
            anchors.fill: parent
            theme: page.theme; panel: sidebar
            handleName: "settingsNavigationHandle"
            separatorName: "settingsNavigationSeparator"
            gripName: "settingsNavigationGrip"

            Item {
                id: sidebar
                objectName: "settingsSidebar"
                SplitView.minimumWidth: 80
                SplitView.maximumWidth: 260
                SplitView.preferredWidth: 260

                Flickable {
                    anchors.fill: parent; anchors.margins: 10
                    contentWidth: width
                    contentHeight: tabButtons.implicitHeight
                    boundsBehavior: Flickable.StopAtBounds
                    interactive: contentHeight > height
                    clip: true
                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AlwaysOff }
                    Column {
                        id: tabButtons
                        width: parent.width
                        spacing: 8

                    SettingsTabButton { objectName: "settingsTab_application"; width: tabButtons.width; text: "Application"; subtitle: "General app preferences"; iconName: "application"; theme: page.theme; labelReveal: page.navigationReveal; selected: page.activeTab === "application"; onClicked: page.activeTab = "application" }
                    SettingsTabButton { objectName: "settingsTab_media"; width: tabButtons.width; text: "Media"; subtitle: "Media server connection"; iconName: "media"; theme: page.theme; labelReveal: page.navigationReveal; selected: page.activeTab === "media"; onClicked: page.activeTab = "media" }
                    SettingsTabButton { objectName: "settingsTab_endpoints"; width: tabButtons.width; text: "Endpoints"; subtitle: "Submission and lookup APIs"; iconName: "endpoints"; theme: page.theme; labelReveal: page.navigationReveal; selected: page.activeTab === "endpoints"; onClicked: page.activeTab = "endpoints" }
                    SettingsTabButton { objectName: "settingsTab_segments"; width: tabButtons.width; text: "Segments"; subtitle: "Manage segment timestamps"; iconName: "segments"; theme: page.theme; labelReveal: page.navigationReveal; selected: page.activeTab === "segments"; onClicked: page.activeTab = "segments" }
                    SettingsTabButton { objectName: "settingsTab_notifications"; width: tabButtons.width; text: "Notifications"; subtitle: "Segment detection alerts"; iconName: "notifications"; theme: page.theme; labelReveal: page.navigationReveal; selected: page.activeTab === "notifications"; onClicked: page.activeTab = "notifications" }
                    }
                }
            }

            Item {
                id: contentPanel
                objectName: "settingsContentPanel"
                SplitView.fillWidth: true
                SplitView.minimumWidth: 450
                clip: true

                Flickable {
                    id: contentScroll
                    anchors.fill: parent
                    anchors.margins: 20
                    contentWidth: width
                    contentHeight: panelContents.implicitHeight
                    boundsBehavior: Flickable.StopAtBounds
                    interactive: contentHeight > height
                    clip: true
                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AlwaysOff }

                    Column {
                        id: panelContents
                        width: contentScroll.width
                        spacing: 16

                        Text {
                            text: page.activeTab === "application" ? "Application"
                                : page.activeTab === "media" ? "Media"
                                : page.activeTab === "endpoints" ? "Endpoints"
                                : page.activeTab === "segments" ? "Segment deletion" : "Notifications"
                            width: parent.width
                            id: sectionHeading
                            objectName: "settingsSectionHeading"
                            font.pixelSize: 17
                            font.bold: true
                            color: page.theme.text
                            Item {
                                objectName: "settingsInfoControl"
                                x: sectionHeading.contentWidth+8; anchors.verticalCenter: parent.verticalCenter
                                width: 18; height: 18
                                Rectangle { anchors.fill: parent; radius: 9; color: page.theme.hover; Text { anchors.centerIn: parent; text: "i"; font.pixelSize: 12; font.bold: true; color: page.theme.text } }
                                HoverHandler { id: sectionHover }
                                AppToolTip { objectName: "settingsSectionTip"; visible: sectionHover.hovered; text: page.sectionHelp; theme: page.theme }
                            }
                        }

                        Rectangle { width: parent.width; height: 1; color: page.theme.border }

                        Column {
                            visible: page.activeTab === "application"
                            width: parent.width
                            spacing: 16

                            RowLayout {
                                width: parent.width
                                Text { Layout.fillWidth: true; text: "Timestamp format"; color: page.theme.text }
                                AppComboBox { objectName: "timestampFormatControl"; width: 110; Layout.preferredWidth: 110; model: ["seconds", "MM:SS", "both"]; currentIndex: Math.max(0, ["seconds", "MM:SS", "both"].indexOf(page.appSettings.timestampFormat)); implicitHeight: 40; onActivated: function(index) { page.appSettings.setTimestampFormat(["seconds", "MM:SS", "both"][index]) } }
                            }
                            RowLayout {
                                width: parent.width
                                Text { Layout.fillWidth: true; text: "TV Show listing type"; color: page.theme.text }
                                AppComboBox { objectName: "listingTypeControl"; Layout.preferredWidth: 150; model: ["Full (with images)", "Basic (no images)"]; implicitHeight: 40; currentIndex: page.appSettings.listingType === "basic" ? 1 : 0; onActivated: function(index) { page.appSettings.setListingType(index === 1 ? "basic" : "full") } }
                            }
                            RowLayout {
                                width: parent.width; spacing: 8
                                Text { Layout.fillWidth: true; font.pixelSize: 15; font.weight: Font.Medium; text: "Intro minimum length"; color: page.theme.text }
                                AppNumberField { objectName: "introMinimumControl"; Layout.preferredWidth: 92; from: 5; to: 120; value: page.appSettings.introMinimumSeconds; onValueModified: page.appSettings.setIntroMinimumSeconds(value) }
                                Text { text: "seconds"; font.pixelSize: 14; color: page.theme.muted }
                                AppButton { text: "Reset"; enabled: page.appSettings.introMinimumSeconds !== 15; theme: page.theme; onClicked: page.appSettings.setIntroMinimumSeconds(15) }
                            }
                            Column {
                                width: parent.width; spacing: 6
                                RowLayout {
                                    width: parent.width
                                    Text { Layout.fillWidth: true; text: "Effort"; color: page.theme.text }
                                    ColumnLayout {
                                        Layout.preferredWidth: 300; Layout.maximumWidth: 300; Layout.fillWidth: false; spacing: 0
                                        Slider {
                                            id: effortSlider; objectName: "detectionResourcesControl"
                                            Layout.fillWidth: true; implicitHeight: 34
                                            from: 0; to: 2; stepSize: 1; snapMode: Slider.SnapAlways
                                            value: Math.max(0, ["light", "medium", "high"].indexOf(page.appSettings.detectionEffort))
                                            Accessible.name: "Detection effort"
                                            Accessible.description: ["Light", "Medium", "High"][Math.round(value)]
                                            onMoved: page.appSettings.setDetectionEffort(["light", "medium", "high"][Math.round(value)])
                                            background: Rectangle {
                                                x: effortSlider.leftPadding; y: (effortSlider.height - height) / 2
                                                width: effortSlider.availableWidth; height: 6; radius: 3; color: page.theme.border
                                                Rectangle { width: effortSlider.visualPosition * parent.width; height: parent.height; radius: 3; color: page.theme.accent }
                                            }
                                            handle: Rectangle {
                                                x: effortSlider.leftPadding + effortSlider.visualPosition * (effortSlider.availableWidth - width)
                                                y: (effortSlider.height - height) / 2; width: 20; height: 20; radius: 10
                                                color: page.theme.accent; border.width: 1
                                                border.color: effortSlider.activeFocus ? page.theme.text : page.theme.border
                                            }
                                        }
                                        RowLayout {
                                            Layout.fillWidth: true
                                            Repeater {
                                                model: ["Light", "Medium", "High"]
                                                Text {
                                                    required property int index; required property string modelData
                                                    Layout.fillWidth: true; horizontalAlignment: index === 0 ? Text.AlignLeft : index === 2 ? Text.AlignRight : Text.AlignHCenter
                                                    text: modelData; font.pixelSize: 12; font.bold: Math.round(effortSlider.value) === index
                                                    color: Math.round(effortSlider.value) === index ? page.theme.text : page.theme.muted
                                                }
                                            }
                                        }
                                    }
                                }
                                Text {
                                    objectName: "detectionEffortDescription"; width: parent.width
                                    color: page.theme.muted; font.pixelSize: 13; wrapMode: Text.WordWrap
                                    text: page.appSettings.detectionEffort === "light"
                                        ? "Background use with fewer audio refinements and a short visual check around credit starts. Applies to new scans."
                                        : page.appSettings.detectionEffort === "high"
                                        ? "Deepest audio and visual refinement, with the most parallel work. Reserves CPU and memory headroom for the system. Applies to new scans."
                                        : "Balanced resource use, with more audio and visual refinement than Light. Applies to new scans."
                                }
                            }
                            LibrarySettings { width: parent.width; theme: page.theme; section: "application"; mediaLibrary: page.mediaLibrary }
                            Rectangle { width: parent.width; height: 1; color: page.theme.border }
                            Column {
                                width: parent.width; spacing: 10
                                Text { text: "Application updates"; font.pixelSize: 15; font.bold: true; color: page.theme.text }
                                RowLayout {
                                    width: parent.width; spacing: 8
                                    Text { Layout.fillWidth: true; text: "Check for updates"; color: page.theme.text }
                                    AppComboBox {
                                        objectName: "updateCheckModeControl"
                                        Layout.preferredWidth: 150; implicitHeight: 40
                                        model: ["Manual", "Automatic"]
                                        enabled: page.applicationUpdates && page.applicationUpdates.enabled
                                        currentIndex: page.applicationUpdates && page.applicationUpdates.automatic ? 1 : 0
                                        onActivated: function(index) { page.applicationUpdates.setAutomatic(index===1) }
                                    }
                                }
                                RowLayout {
                                    width: parent.width; spacing: 8
                                    Text { Layout.fillWidth: true; text: page.applicationUpdates ? "Version "+page.applicationUpdates.installedVersion : "Introvert"; color: page.theme.muted }
                                    AppButton { objectName: "checkApplicationUpdatesControl"; text: "Check for Updates"; theme: page.theme; enabled: page.applicationUpdates && page.applicationUpdates.enabled && !page.applicationUpdates.busy && !page.applicationUpdates.ready; onClicked: page.applicationUpdates.check() }
                                    AppButton {
                                        objectName: "installApplicationUpdateControl"
                                        visible: page.applicationUpdates && page.applicationUpdates.availableVersion!=="" && page.applicationUpdates.updateKind==="application"
                                        enabled: page.applicationUpdates && !page.applicationUpdates.busy
                                        text: page.applicationUpdates && page.applicationUpdates.ready ? "Restart to Install" : "Download Update"
                                        theme: page.theme
                                        onClicked: {if(page.applicationUpdates.ready)page.applicationUpdates.install();else page.applicationUpdates.download()}
                                    }
                                    AppButton { objectName: "cancelApplicationUpdateControl"; visible: page.applicationUpdates && page.applicationUpdates.busy; text: "Cancel"; theme: page.theme; onClicked: page.applicationUpdates.cancel() }
                                }
                                RowLayout {
                                    width: parent.width; spacing: 8
                                    Text { objectName: "detectorVersionLabel"; Layout.fillWidth: true; text: page.applicationUpdates ? "Detector "+page.applicationUpdates.detectorInstalledVersion : "Detector"; color: page.theme.muted }
                                    AppButton {
                                        objectName: "installDetectorUpdateControl"
                                        visible: page.applicationUpdates && page.applicationUpdates.detectorAvailableVersion!==""
                                        enabled: page.applicationUpdates && !page.applicationUpdates.busy && !page.applicationUpdates.ready
                                        text: "Download Detector Update"; theme: page.theme
                                        onClicked: page.applicationUpdates.downloadDetector()
                                    }
                                }
                                Text { objectName: "applicationUpdateStatus"; width: parent.width; text: page.applicationUpdates ? page.applicationUpdates.message : "Update checks are unavailable."; wrapMode: Text.Wrap; color: page.theme.muted; font.pixelSize: 13 }
                                Rectangle {
                                    visible: page.applicationUpdates && page.applicationUpdates.busy && page.applicationUpdates.progress>0
                                    width: parent.width; height: 3; color: page.theme.field
                                    Rectangle { width: parent.width*(page.applicationUpdates ? page.applicationUpdates.progress/100 : 0); height: parent.height; color: page.theme.accent }
                                }
                            }
                        }

                        Column {
                            visible: page.activeTab === "media"
                            width: parent.width
                            spacing: 14
                            Column { width: parent.width; spacing: 6
                                Text { text: "Media"; font.pixelSize: 15; font.bold: true; color: page.theme.text }
                                AppComboBox {
                                    objectName: "mediaProviderControl"
                                    implicitHeight: 40
                                    width: parent.width
                                    model: ["Not configured", "Plex", "Emby", "Jellyfin", "Local"]
                                    currentIndex: Math.max(0, ["", "plex", "emby", "jellyfin", "local"].indexOf(page.appSettings.mediaProvider))
                                    onActivated: function(index) { page.appSettings.setMediaProvider(["", "plex", "emby", "jellyfin", "local"][index]) }
                                }
                            }
                            Column {
                                width: parent.width
                                spacing: 12
                                visible: page.appSettings.mediaProvider !== ""
                                Column { width: parent.width; spacing: 6
                                    Text {
                                        font.pixelSize: 15; font.bold: true
                                        text: page.appSettings.mediaProvider === "local" ? "Local media path" : "URL"
                                        color: page.theme.text
                                    }
                                    Row { width: parent.width; spacing: 8
                                        AppTextField {
                                            objectName: "mediaAddressControl"
                                            height: 40
                                            width: parent.width - (page.appSettings.mediaProvider === "local" ? 88 : 0)
                                            text: page.appSettings.mediaSettings.address
                                            placeholderText: page.appSettings.mediaProvider === "local" ? "/Volumes/TV" : page.appSettings.mediaProvider === "plex" ? "http://server:32400" : "http://server:8096"
                                            onTextEdited: page.appSettings.mediaSettings.setAddress(text)
                                        }
                                        AppButton { objectName: "localBrowseControl"; visible: page.appSettings.mediaProvider === "local"; text: "Browse"; theme: page.theme; width: 80; height: 40; onClicked: localFolder.open() }
                                    }
                                }
                                Column {
                                    width: parent.width; spacing: 6
                                    visible: page.appSettings.mediaProvider !== "local"
                                    Text { text: "API Key"; color: page.theme.text; font.pixelSize: 15; font.bold: true }
                                    Row {
                                        width: parent.width; spacing: 8
                                        AppTextField {
                                            id: credentialEntry
                                            objectName: "mediaCredentialControl"
                                            property bool reveal: false
                                            property bool edited: false
                                            width: parent.width-48; height: 40
                                            echoMode: reveal ? TextInput.Normal : TextInput.Password
                                            inputMethodHints: Qt.ImhHiddenText | Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
                                            placeholderText: ""
                                            onTextEdited: {
                                                edited=true
                                                page.appSettings.mediaSettings.setCredentialInput(text)
                                            }
                                        }
                                        AppButton {
                                            objectName: "mediaCredentialReveal"
                                            width: 40; height: 40; theme: page.theme
                                            iconName: credentialEntry.reveal ? "eye-off" : "eye"
                                            hint: credentialEntry.reveal ? "Hide typed API key" : "Show typed API key"
                                            onClicked: credentialEntry.reveal=!credentialEntry.reveal
                                        }
                                    }
                                }
                                CheckBox {
                                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                                    implicitHeight: 22; padding: 0
                                    indicator: Rectangle { width: 14; height: 14; y: (parent.height-height)/2; radius: 3; color: parent.checked ? "#00a9ad" : page.theme.field; border.color: page.theme.border; Text { anchors.centerIn: parent; text: "✓"; font.pixelSize: 12; color: "white"; visible: parent.parent.checked } }
                                    contentItem: Text { leftPadding: 22; text: "Fetch images"; font.pixelSize: 14; color: page.theme.text; verticalAlignment: Text.AlignVCenter }
                                    objectName: "mediaImagesControl"
                                    visible: page.appSettings.mediaProvider === "local"
                                    text: "Fetch images"
                                    checked: page.appSettings.mediaSettings.fetchImages
                                    onToggled: page.appSettings.mediaSettings.setFetchImages(checked)
                                }
                                Rectangle { width: parent.width; height: 1; color: page.theme.border }
                                AppButton {
                                    objectName: "rebuildMediaCacheControl"
                                    visible: ["plex","emby","jellyfin"].indexOf(page.appSettings.mediaProvider)>=0
                                    readonly property bool rebuilding: Boolean(page.mediaLibrary && page.mediaLibrary.rebuildingCache)
                                    text: rebuilding ? "Rebuilding Cache…" : "Rebuild Cache"
                                    implicitWidth: 180; theme: page.theme
                                    enabled: !page.preview && Boolean(page.mediaLibrary) && !page.mediaLibrary.busy && !rebuilding
                                    opacity: enabled || rebuilding ? 1 : .45
                                    progress: rebuilding ? page.mediaLibrary.cacheRebuildProgress : -1
                                    onClicked: page.mediaLibrary.rebuildCache()
                                }
                                Text {
                                    objectName: "mediaConfigurationError"
                                    visible: text.length>0
                                    text: page.appSettings.mediaSettings.message
                                    width: parent.width; wrapMode: Text.WordWrap; color: page.theme.warning
                                }
                                Connections {
                                    target: page.appSettings.mediaSettings
                                    function onSaved() { credentialEntry.reveal=false }
                                    function onCleared() {
                                        credentialEntry.text=page.appSettings.mediaSettings.credentialInput
                                        credentialEntry.reveal=false
                                        credentialEntry.edited=page.appSettings.mediaSettings.credentialEdited
                                    }
                                }
                            }
                        }

                        LibrarySettings { id: localExtras; visible: page.activeTab === "media" && page.appSettings.mediaProvider === "local"; width: parent.width; theme: page.theme; section: "media"; localProvider: true; mediaLibrary: page.mediaLibrary }
                        ServiceSettings {
                            visible: page.activeTab !== "application" && page.activeTab !== "media"
                            width: parent.width
                            theme: page.theme; endpointSettings: page.appSettings.endpointSettings; webhookSettings: page.appSettings.webhookSettings; segmentManager: page.segmentManager; section: page.activeTab; preview: page.preview; shows: page.previewShows
                        }
                    }
                }
            }
        }
    }
    readonly property string sectionHelp: page.activeTab === "application" ? "Introvert detects recurring intros and credits across a season. Use Detect Segments on a show to scan its selected season. Display preferences are saved automatically." : page.activeTab === "media" || page.activeTab === "endpoints" ? "Settings and API keys are saved in the local app.db." : page.activeTab === "notifications" ? "Webhook settings and the optional key are saved in app.db. Enabled webhooks receive successful detection summaries." : "Manage saved segment timestamps."
}
