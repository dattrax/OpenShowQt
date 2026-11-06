import QtQuick
import QtQuick.Controls

// Installation and audience routing live here. Show contains runtime cues.
ScrollView {
    id: settingsPage
    objectName: "settingsPage"
    contentWidth: availableWidth
    clip: true
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

    /* --- the service's transition (task TR.3, shared in CS.5) --------------
     * The standing answer to "how does the audience get to the next cue", and
     * therefore what a cue resolves to when no theme and no Bible view speaks
     * for itself. The controls are the SHARED TransitionControls part over an
     * InspectorAdapter aimed at the service floor: kind and both durations
     * travel together as ONE record, because a transition is atomic — the
     * file, the wire and the domain all refuse a fragment — so a half commit
     * is not something this page can do by accident, and one gesture costs
     * one undo.
     *
     * Assignment rather than binding, syncing off the adapter's changed(), a
     * refused write snapping the controls back to the model, and a Cut
     * DISABLING the two numbers rather than clearing them — all the part's
     * rules, identical to the Theme Editor's rails. The floor is concrete:
     * no Inherit entry, no own/inherited badge and no source sentence here,
     * and the picker explains itself inline.
     */
    InspectorAdapter {
        id: serviceAdapter
        document: docs
        target: ({kind: "service"})
    }

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
        Text { text: "TRANSITION"; color: "#bbbbbb"; font.pixelSize: 11; font.bold: true }
        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            color: "#b0b0b0"; font.pixelSize: 12
            text: "How the audience moves to the next cue. A theme or the Bible view can answer "
                + "for itself in the Theme Editor; this is what a cue with nothing above it does."
        }
        // The shared movement part (task CS.5): same rules and same adapter
        // vocabulary as the Theme Editor's rails, aimed at the service floor
        // — no Inherit, no badge, no source sentence, and the page's own
        // heading and note stay here because they describe more than the
        // part draws.
        TransitionControls {
            adapter: serviceAdapter
            allowInherit: false
            showKindHint: true
            showOwnBadge: false
            showSource: false
            sectionTitle: ""
            labelPixelSize: 12
            backLabel: "Background duration (ms)"
            kindObjectName: "serviceTransitionKind"
            wordsObjectName: "serviceTransitionWords"
            backObjectName: "serviceTransitionBack"
            noteObjectName: "serviceTransitionNote"
            note: "Words covers the text and any foreground picture; the background is a "
                + "colour, a gradient or a still image. A background that is a VIDEO still cuts "
                + "either way — a crossfade there would composite two stills over decoders that "
                + "have not produced frames — so no background number dissolves one clip into "
                + "the next. 0 ms lands that track on its first frame, and a Cut ignores both "
                + "numbers without forgetting them. What is set here is the NEXT cue's move: a "
                + "fade already crossing keeps the lengths it was given when Go was pressed."
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
                {key: "Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z", what: "Undo / redo in Edit or the focused Library/service list"},
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
