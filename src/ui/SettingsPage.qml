import QtQuick
import QtQuick.Controls

// Installation and audience routing live here. Show contains runtime cues.
ScrollView {
    id: settingsPage
    objectName: "settingsPage"
    contentWidth: availableWidth
    clip: true
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

    /* --- the service's transition (task TR.3) ------------------------------
     * The standing answer to "how does the audience get to the next cue", and
     * therefore what a cue resolves to when no theme and no Bible view speaks
     * for itself. Three controls, ONE record: kind and both durations travel
     * together through the controller's verb, because a transition is atomic —
     * the file, the wire and the domain all refuse a fragment — so a half
     * commit is not something this page can do by accident, and one gesture
     * costs one undo.
     *
     * Assignment rather than binding, exactly as the Theme Editor's fields do
     * it: a number the operator is typing must not be overwritten by an
     * unrelated change to the documents, and every successful commit re-reads
     * the whole record from the model anyway. A refused write — a value that
     * is not a whole count of milliseconds, or one outside the domain's band —
     * puts every control back where the model says it belongs, because a field
     * showing a number the service turned down is a field about to be trusted.
     *
     * A Cut DISABLES the two numbers rather than clearing them. Choosing Cut
     * for an emergency and Crossfade again returns to the lengths that were
     * there, which is honest: the record underneath still says those numbers,
     * and a Cut is a decision about the kind, not a promise to forget the
     * times.
     */
    // What the model holds, kept beside the controls so a commit can tell
    // "focus moved through a field" from "the operator changed an answer" —
    // the first writes nothing and costs no undo.
    property var transitionRecord: ({kind: "fade", textDurationMs: 500, mediaDurationMs: 800})
    function syncTransition() {
        const record = docs.serviceDefaultTransition()
        settingsPage.transitionRecord = record
        transitionKind.currentIndex = record.kind === "cut" ? 1 : 0
        transitionWords.text = String(record.textDurationMs)
        transitionBack.text = String(record.mediaDurationMs)
    }
    // A typed millisecond count, or -1 for "that is not a duration". Only the
    // SHAPE is checked here: digits, no sign, no decimal point. The BAND is
    // the domain's one sentence, so a 99999 this page lets through is the
    // verb's to refuse — and the refusal re-reads the record below, so the
    // field snaps back to what the service holds rather than to a ceiling this
    // page might have forgotten to update.
    function msOr(textIn) {
        const t = String(textIn).trim()
        if (t === "" || !/^[0-9]+$/.test(t))
            return -1
        return Math.round(Number(t))
    }
    // Whichever control moved, the whole record commits: the kind the picker
    // shows and both numbers as the page shows them.
    function commitTransitionFields() {
        const kind = transitionKind.currentIndex === 1 ? "cut" : "fade"
        const words = settingsPage.msOr(transitionWords.text)
        const back = settingsPage.msOr(transitionBack.text)
        if (words < 0 || back < 0) {
            settingsPage.syncTransition()  // not a number: the model's numbers win
            return
        }
        const held = settingsPage.transitionRecord
        if (held.kind === kind && held.textDurationMs === words
                && held.mediaDurationMs === back)
            return  // focus moved with nothing changed: no write, no undo
        // A refusal has no documentsChanged to repaint from, so re-read here;
        // a success repaints through the model's own emit.
        if (!docs.setServiceDefaultTransition(kind, words, back))
            settingsPage.syncTransition()
    }
    // The record belongs to the service, so whatever moved it — this page, an
    // undo, a reload — the controls follow the model, not their own last
    // keystroke.
    Connections {
        target: docs
        function onDocumentsChanged() {
            settingsPage.syncTransition()
        }
    }
    Component.onCompleted: settingsPage.syncTransition()

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
        Row {
            spacing: 8
            ComboBox {
                id: transitionKind
                objectName: "serviceTransitionKind"
                model: ["Crossfade", "Cut"]
                onActivated: settingsPage.commitTransitionFields()
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                color: "#888888"; font.pixelSize: 11
                text: transitionKind.currentIndex === 1
                        ? "no dissolve — the new cue is simply on"
                        : "both cues drawn, blended over the times below"
            }
        }
        Text { text: "Words duration (ms, 0 to 10000)"; color: "#b0b0b0"; font.pixelSize: 12 }
        PanelField {
            id: transitionWords
            objectName: "serviceTransitionWords"
            width: parent.width
            enabled: transitionKind.currentIndex === 0
            onEditingFinished: settingsPage.commitTransitionFields()
        }
        Text { text: "Background duration (ms)"; color: "#b0b0b0"; font.pixelSize: 12 }
        PanelField {
            id: transitionBack
            objectName: "serviceTransitionBack"
            width: parent.width
            enabled: transitionKind.currentIndex === 0
            onEditingFinished: settingsPage.commitTransitionFields()
        }
        Text {
            objectName: "serviceTransitionNote"
            width: parent.width
            wrapMode: Text.WordWrap
            color: "#888888"; font.pixelSize: 10
            text: "Words covers the text and any foreground picture; the background is a "
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
