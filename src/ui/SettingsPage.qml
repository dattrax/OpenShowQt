import QtQuick
import QtQuick.Controls

// Installation and audience routing live here. Show contains runtime cues.
ScrollView {
    id: settingsPage
    objectName: "settingsPage"
    contentWidth: availableWidth
    clip: true
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

    Column {
        width: settingsPage.availableWidth
        spacing: 12
        Text { text: "Settings"; color: "#eeeeee"; font.pixelSize: 18; font.bold: true }
        Text { text: "AUDIENCE DISPLAY"; color: "#bbbbbb"; font.pixelSize: 11; font.bold: true }
        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            color: "#b0b0b0"; font.pixelSize: 12
            text: "Choose where the audience picture is sent. Changing this moves the audience window."
        }
        ComboBox {
            id: destination
            objectName: "audienceDestinationPicker"
            width: parent.width
            enabled: worker.outputLive
            model: [{value: -1, label: "Windowed"}].concat(
                worker.displays.map(d => ({value: d.index, label: d.name + " — " + d.size})))
            textRole: "label"; valueRole: "value"
            currentIndex: destination.indexOfValue(worker.currentMonitor)
            onActivated: worker.selectDisplay(currentValue)
        }
        Text { text: "AUDIENCE CONTENT"; color: "#bbbbbb"; font.pixelSize: 11; font.bold: true }
        Column {
            width: parent.width
            enabled: worker.outputLive
            Repeater {
                model: [{key: "background", label: "Show background media"},
                        {key: "images", label: "Show slide images"},
                        {key: "words", label: "Show slide text"}]
                delegate: CheckBox {
                    required property var modelData
                    objectName: "audienceContentSetting"
                    text: modelData.label
                    font.pixelSize: 12
                    checked: worker.layers[modelData.key] === true
                    onToggled: worker.setLayerEnabled(modelData.key, checked)
                }
            }
        }
        Text { text: "PRESENTER"; color: "#bbbbbb"; font.pixelSize: 11; font.bold: true }
        CheckBox {
            text: "Number keys play slides (1–9, 0 = ten)"
            font.pixelSize: 12
            checked: appSettings.numberKeysPresent
            onToggled: appSettings.numberKeysPresent = checked
        }
        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            color: "#b0b0b0"; font.pixelSize: 12
            text: "Presenter keys run the current presentation in Show. Edit keeps content changes separate from presenting."
        }
        Text { text: "OPERATOR WINDOW"; color: "#bbbbbb"; font.pixelSize: 11; font.bold: true }
        CheckBox {
            text: "Fullscreen (F11)"
            font.pixelSize: 12
            checked: appSettings.fullscreen
            onToggled: appSettings.fullscreen = checked
        }
        Text { text: "SHORTCUTS"; color: "#bbbbbb"; font.pixelSize: 11; font.bold: true }
        Repeater {
            model: [
                {key: "Space / PageDown / Right / F5", what: "Next slide in Show — straight to the output"},
                {key: "PageUp / Left", what: "Previous slide"},
                {key: "Home / End", what: "First / last slide"},
                {key: ".", what: "Clear everything on air"},
                {key: "Esc", what: "Clear all or restore; deselect first in Edit"},
                {key: "F1 / F2", what: "Clear the background / slide"},
                {key: "Ctrl+1 / Ctrl+2 / Ctrl+5", what: "Show, Edit and Settings"},
                {key: "Ctrl+F", what: "Search presentations (opens the sidebar's field)"},
                {key: "Ctrl+D", what: "Duplicate in Edit; toggle the media bin elsewhere"},
                {key: "Ctrl+C / Ctrl+X / Ctrl+V", what: "Copy slide; cut / paste in Edit only"},
                {key: "Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z", what: "Undo / redo in Edit"},
                {key: "Ctrl+S", what: "Save the service"},
                {key: "Arrows / Ctrl+Arrows", what: "Nudge the selected box 1px / 10px in Edit"},
                {key: "Tab / Shift+Tab", what: "Select next / previous box in Edit"},
                {key: "F11", what: "Toggle operator fullscreen"}
            ]
            delegate: Column {
                required property var modelData
                width: settingsPage.availableWidth
                spacing: 2
                Text { width: parent.width; wrapMode: Text.WordWrap; text: modelData.key; color: "#eeeeee"; font.pixelSize: 12 }
                Text { width: parent.width; wrapMode: Text.WordWrap; text: modelData.what; color: "#b0b0b0"; font.pixelSize: 12 }
            }
        }
        Text { text: "SERVICE AND DIAGNOSTICS"; color: "#bbbbbb"; font.pixelSize: 11; font.bold: true }
        Text {
            width: parent.width; wrapMode: Text.WrapAnywhere
            color: "#b0b0b0"; font.pixelSize: 12
            text: "Version: " + applicationVersion + "\nConnection: " + worker.status
                + "\nGraphics: " + worker.device + "\nService folder: " + docs.serviceDirectory
                + "\n" + docs.status
        }
        Text {
            width: parent.width; wrapMode: Text.WrapAnywhere
            visible: worker.mediaError !== "" || worker.standaloneError !== "" || worker.foregroundErrors.length > 0
            color: "#d9a441"; font.pixelSize: 12
            text: [worker.mediaError, worker.standaloneError]
                .concat(worker.foregroundErrors.map(f => f.path + " — " + f.error))
                .filter(message => message !== "").join("\n")
        }
        Text {
            width: parent.width; wrapMode: Text.WordWrap
            color: "#b0b0b0"; font.pixelSize: 12
            text: "Recovery autosave follows edits. Ctrl+S writes the service package."
        }
    }
}
