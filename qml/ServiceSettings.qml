import QtQuick
Column {
    id: panel
    required property var theme
    property string section: ""
    property bool preview: false
    property var segmentManager: null
    property var endpointSettings: null
    property var webhookSettings: null
    property var notificationSettings: null
    property var shows: []
    spacing: section === "endpoints" ? 12 : 16
    readonly property var fields: [
        {kind:"heading",label:"introdb.app"},{kind:"toggle",label:"Enabled",key:"introdb_enabled"},{kind:"url",label:"URL",key:"introdb_api_url",value:"https://api.introdb.app/submit"},{kind:"url",label:"Lookup URL",key:"introdb_lookup_url",value:"https://api.introdb.app/segments"},{kind:"secret",label:"API Key",key:"introdb_app"},
        {kind:"heading",label:"theintrodb.org"},{kind:"toggle",label:"Enabled",key:"theintrodb_enabled"},{kind:"url",label:"URL",key:"theintrodb_base_url",value:"https://api.theintrodb.org/v3"},{kind:"secret",label:"API Key",key:"theintrodb_org"},
        {kind:"heading",label:"Global submission settings"},{kind:"number",label:"API Timeout (ms)",key:"api_timeout_ms",value:"10000"},{kind:"number",label:"API Delay (seconds)",key:"api_delay_seconds",value:"-1"},{kind:"toggle",label:"Save submissions in local DB",key:"save_submissions_local_db"}
    ]
    Repeater {
        model: panel.fields
        Loader {
            visible: panel.section === "endpoints"
            required property var modelData
            required property int index
            width: panel.width
            property var fieldData: modelData
            property bool firstField: index === 0
            sourceComponent: modelData.kind === "heading" ? heading : field
            Component { id: heading; Column { width: parent.width; spacing: 14; Rectangle { visible: !firstField; width: parent.width; height: 1; color: panel.theme.border } Text { text: fieldData.label; font.bold: true; font.pixelSize: 15; color: panel.theme.text } } }
            Component {
                id: field
                FormRow {
                    id: endpointRow
                    objectName: "endpointField_"+fieldData.key
                    compact: panel.section === "endpoints"; width: parent.width; theme: panel.theme
                    label: fieldData.label; fieldKey: fieldData.key; kind: fieldData.kind || "text"
                    enabled: Boolean(panel.endpointSettings)
                    value: kind === "secret" ? "" : panel.endpointSettings ? String(panel.endpointSettings.values[fieldData.key]) : fieldData.value || ""
                    checked: panel.endpointSettings ? Boolean(panel.endpointSettings.values[fieldData.key]) : false
                    secretSaved: kind === "secret" && panel.endpointSettings ? Boolean(panel.endpointSettings.keyStatus[fieldData.key]) : false
                    Connections { target: panel.endpointSettings || null; function onSaved() { endpointRow.reveal=false } }
                    onValueEdited: function(value) {
                        if(!panel.endpointSettings) return
                        if(kind === "secret") panel.endpointSettings.editKey(fieldData.key,value)
                        else panel.endpointSettings.setValue(fieldData.key,value)
                    }
                }
            }
        }
    }
    Column {
        objectName: "inAppNotificationSection"
        visible: panel.section === "notifications"; width: parent.width; spacing: 12
        Text { text: "In-app notifications"; color: panel.theme.text; font.pixelSize: 15; font.bold: true }
        Text { width: parent.width; text: "Choose which alerts appear inside Introvert."; color: panel.theme.muted; font.pixelSize: 13; wrapMode: Text.Wrap }
        Item {
            width: parent.width; height: 34
            Text { anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter; text: "Enabled"; color: panel.theme.text; font.pixelSize: 15; font.bold: true }
            SegmentSwitch {
                objectName: "toggle_in_app_enabled"
                anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                enabled: Boolean(panel.notificationSettings)
                checked: panel.notificationSettings ? panel.notificationSettings.preferences.enabled : true
                Accessible.name: "Enable in-app notifications"
                onToggled: if(panel.notificationSettings) panel.notificationSettings.setPreference("enabled", checked)
            }
        }
        Column {
            width: parent.width; spacing: 4
            enabled: Boolean(panel.notificationSettings && panel.notificationSettings.preferences.enabled)
            opacity: enabled ? 1 : .45
            Repeater {
                model: [{key:"detection_complete", label:"Detection completion"}, {key:"errors", label:"Errors"},
                        {key:"application_updates", label:"Application updates"}, {key:"detector_updates", label:"Detector updates"}]
                Item {
                    required property var modelData
                    width: parent.width; height: 34
                    Text { anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter; text: parent.modelData.label; color: panel.theme.text; font.pixelSize: 14 }
                    SegmentSwitch {
                        objectName: "toggle_in_app_" + parent.modelData.key
                        anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                        checked: panel.notificationSettings ? Boolean(panel.notificationSettings.preferences[parent.modelData.key]) : true
                        Accessible.name: parent.modelData.label + " notifications"
                        onToggled: if(panel.notificationSettings) panel.notificationSettings.setPreference(parent.modelData.key, checked)
                    }
                }
            }
        }
        Text { width: parent.width; text: "Update alerts appear when automatic update checks are enabled."; color: panel.theme.muted; font.pixelSize: 12; wrapMode: Text.Wrap }
        Rectangle { width: parent.width; height: 1; color: panel.theme.border }
    }
    Column {
        objectName: "webhookNotificationSection"
        visible: panel.section === "notifications"; width: parent.width; spacing: 16
        enabled: Boolean(panel.webhookSettings)
        Text { text: "Webhook notifications"; color: panel.theme.text; font.pixelSize: 15; font.bold: true }
        Item {
            width: parent.width; height: 34
            Text { anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter; text: "Enabled"; color: panel.theme.text; font.pixelSize: 15; font.bold: true }
            SegmentSwitch { objectName: "toggle_webhook_enabled"; anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; checked: panel.webhookSettings ? Boolean(panel.webhookSettings.values.enabled) : false; onToggled: panel.webhookSettings.setValue("enabled",checked) }
        }
        AppButton { objectName: "testWebhookControl"; anchors.right: parent.right; text: "Test"; enabled: !panel.preview && Boolean(panel.webhookSettings) && panel.webhookSettings.canTest; theme: panel.theme; onClicked: panel.webhookSettings.test() }
        FormRow {
            width: parent.width; theme: panel.theme; label: "URL"; fieldKey: "webhook_url"
            value: panel.webhookSettings ? panel.webhookSettings.values.url : ""
            onValueEdited: function(value) { if(panel.webhookSettings) panel.webhookSettings.setValue("url",value) }
        }
        FormRow {
            id: webhookKey
            Connections { target: panel.webhookSettings || null; function onSaved() { webhookKey.reveal=false } }
            width: parent.width; theme: panel.theme; label: "Key (optional)"; fieldKey: "webhook_key"; kind: "secret"
            secretSaved: panel.webhookSettings ? panel.webhookSettings.hasKey : false
            onValueEdited: function(value) { if(panel.webhookSettings) panel.webhookSettings.editKey(value) }
        }
        Rectangle { width: parent.width; height: 1; color: panel.theme.border }
        Text { text: "Example: Seinfeld • S01 • 24 segments • 4.5 min"; color: panel.theme.muted; font.pixelSize: 13 }
    }
    SegmentBrowser { visible: panel.section === "segments"; width: parent.width; height: 510; theme: panel.theme; segmentManager: panel.segmentManager; preview: panel.preview; shows: panel.shows }
    Rectangle { visible: panel.section !== "segments"; width: parent.width; height: 1; color: panel.theme.border }
    Text { visible: panel.section !== "segments"; width: parent.width; text: panel.section === "endpoints" && panel.endpointSettings ? panel.endpointSettings.message : panel.section === "notifications" && panel.webhookSettings ? panel.webhookSettings.message : ""; color: panel.theme.muted; font.pixelSize: 13; wrapMode: Text.WordWrap }
}
