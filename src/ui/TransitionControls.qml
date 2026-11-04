import QtQuick
import QtQuick.Controls

/*
 * UI part: TransitionControls; contract: docs/ui/control-sets.md#transitioncontrols
 *
 * How a cue wearing THIS target arrives (task CS.5): the picker's choices,
 * both durations, and — for the targets that can be silent — the Inherit
 * entry and the source sentence naming the seat whose numbers are on show.
 * Extracted verbatim in behaviour from ThemeEditorPage's two transition
 * sections (task TR.3) and SettingsPage's service section (task TR.2):
 *
 *   - A transition is ONE atomic record — kind plus both durations travel
 *     together, on the file, the wire and the domain — so whichever
 *     control moved, the whole record commits, and one gesture costs one
 *     undo.
 *   - Inherit writes NOTHING: the record is removed, which is a different
 *     act from storing a copy of what silence resolves to. It is offered
 *     only against a record that exists (allowInherit targets only).
 *   - Cut and Crossfade each write a WHOLE record, seeded from the numbers
 *     on show — nobody gets a silent 0.
 *   - A Cut DISABLES the durations without erasing them: the record still
 *     holds them, so coming back to Crossfade comes back to the times.
 *   - QML validates SHAPE only (digits, no sign, no decimal); the band is
 *     the domain's one sentence, so a 99999 this part lets through is the
 *     verb's to refuse — and the refusal re-reads the record, putting the
 *     fields back at what the model holds rather than at a ceiling this
 *     page might have forgotten to update.
 *   - An unchanged commit writes nothing and costs no undo.
 */
Column {
    id: move

    required property InspectorAdapter adapter
    // The service floor is concrete — no Inherit entry there, no own/
    // inherited badge, no source sentence.
    property bool allowInherit: true
    property bool showKindHint: false      // Settings explains the choice inline
    property bool showOwnBadge: true
    property bool showSource: true
    // Appended to the source sentence when the target is silent, naming
    // how much further down a Go would walk from here.
    property string silentSuffix: ""
    property string wordsLabel: "Words duration (ms, 0 to 10000)"
    property string backLabel: "Background duration (ms, 0 to 10000)"
    property int labelPixelSize: 11
    // Section header; "" hides it (Settings has its own page heading).
    property string sectionTitle: "Transition"
    // The host's standing note beside these controls ("" = none).
    property string note: ""

    property string kindObjectName: "transitionKind"
    property string inheritObjectName: "transitionInherit"
    property string wordsObjectName: "transitionWords"
    property string backObjectName: "transitionBack"
    property string sourceObjectName: "transitionSource"
    property string noteObjectName: "transitionNote"

    spacing: 8
    width: parent ? parent.width : 0

    function state() { return move.adapter.fieldState("transition") }
    // Cut is the model's second entry in both spellings; Crossfade is the
    // third with Inherit on top and the first without it.
    function cutIndex() { return 1 }
    function indexOf(kind, isSet) {
        if (move.allowInherit)
            return isSet !== true ? 0 : (kind === "cut" ? 1 : 2)
        return kind === "cut" ? 1 : 0
    }
    function describe(s) {
        const v = s.value
        if (v === undefined || v === null || v.kind === undefined)
            return ""
        const who = s.isSet === true ? s.source + " answers for itself"
                                     : "inherits " + s.source
        if (v.kind === "cut")
            return who + " — Cut: the cue is simply on"
        return who + " — Crossfade " + v.textDurationMs + " ms words / "
               + v.mediaDurationMs + " ms background"
    }
    function sync() {
        const s = move.state()
        const v = s.value
        const live = v !== undefined && v !== null && v.kind !== undefined
        kindCombo.currentIndex = live ? move.indexOf(v.kind, s.isSet) : 0
        wordsField.text = live ? String(v.textDurationMs) : ""
        backField.text = live ? String(v.mediaDurationMs) : ""
        // The durations edit only this target's own fade record — a Cut
        // ignores them, silence has none of its own.
        const ownFade = s.editable === true && s.isSet === true && live
                        && v.kind === "fade"
        wordsField.enabled = ownFade
        backField.enabled = ownFade
        inheritButton.enabled = s.canReset === true
        ownBadge.text = s.isSet === true ? "own" : "inherited"
        ownBadge.color = s.isSet === true ? "#8ec9f5" : "#888888"
        sourceText.text = describe(s) + (s.isSet === true ? "" : move.silentSuffix)
        sourceText.color = s.isSet === true ? "#8ec9f5" : "#888888"
        kindCombo.enabled = s.editable === true
    }
    // A typed millisecond count, or -1 for "that is not a duration" —
    // shape only; the BAND is the domain's.
    function msOr(textIn) {
        const t = String(textIn).trim()
        if (t === "" || !/^[0-9]+$/.test(t))
            return -1
        return Math.round(Number(t))
    }
    // Whichever control moved, the whole record commits — or nothing does.
    function commitFromView() {
        const s = move.state()
        if (move.allowInherit && kindCombo.currentIndex === 0) {
            // Inherit is the REMOVAL, and only ever offered against a
            // record that exists — writing "nothing" over nothing would
            // cost an undo for no change.
            if (s.canReset === true && !move.adapter.reset("transition"))
                move.sync()
            return
        }
        const kind = kindCombo.currentIndex === move.cutIndex() ? "cut" : "fade"
        const words = move.msOr(wordsField.text)
        const back = move.msOr(backField.text)
        if (words < 0 || back < 0) {
            move.sync()  // not a count of ms: the model's numbers win
            return
        }
        const v = s.value
        if (s.isSet === true && v !== undefined && v !== null
                && v.kind === kind && v.textDurationMs === words
                && v.mediaDurationMs === back)
            return  // focus moved with nothing changed: no write, no undo
        if (!move.adapter.commit("transition", {kind: kind, textDurationMs: words,
                                                mediaDurationMs: back}))
            move.sync()
    }

    Connections {
        target: move.adapter
        function onChanged() { move.sync() }
        function onTargetChanged() { move.sync() }
    }
    Component.onCompleted: move.sync()

    Text {
        visible: move.sectionTitle !== ""
        text: move.sectionTitle
        color: "#bbbbbb"
        font.pixelSize: 11
        font.bold: true
    }
    Row {
        spacing: 8
        ComboBox {
            id: kindCombo
            objectName: move.kindObjectName
            width: move.showKindHint ? implicitWidth : parent.width
            model: move.allowInherit ? ["Inherit", "Cut", "Crossfade"]
                                     : ["Crossfade", "Cut"]
            onActivated: move.commitFromView()
        }
        Text {
            visible: move.showKindHint
            anchors.verticalCenter: parent.verticalCenter
            color: "#888888"; font.pixelSize: 11
            text: kindCombo.currentIndex === 1
                    ? "no dissolve — the new cue is simply on"
                    : "both cues drawn, blended over the times below"
        }
    }
    Row {
        visible: move.showOwnBadge && move.allowInherit
        spacing: 6
        PanelButton {
            id: inheritButton
            objectName: move.inheritObjectName
            text: "Inherit"
            onClicked: {
                if (!move.adapter.reset("transition"))
                    move.sync()
            }
        }
        Text {
            id: ownBadge
            anchors.verticalCenter: parent.verticalCenter
            font.pixelSize: 10
            color: "#888888"
            text: "inherited"
        }
    }
    Text { text: move.wordsLabel; color: "#b0b0b0"; font.pixelSize: move.labelPixelSize }
    PanelField {
        id: wordsField
        objectName: move.wordsObjectName
        width: parent.width
        // A Cut ignores the numbers and Inherit has none of its own to
        // edit — both DISABLE the fields WITHOUT clearing them, because
        // the record still holds what is shown.
        onEditingFinished: move.commitFromView()
    }
    Text { text: move.backLabel; color: "#b0b0b0"; font.pixelSize: move.labelPixelSize }
    PanelField {
        id: backField
        objectName: move.backObjectName
        width: parent.width
        onEditingFinished: move.commitFromView()
    }
    Text {
        id: sourceText
        objectName: move.sourceObjectName
        visible: move.showSource
        width: parent.width
        wrapMode: Text.WordWrap
        font.pixelSize: 10
        color: "#888888"
    }
    Text {
        visible: move.note !== ""
        objectName: move.noteObjectName
        width: parent.width
        wrapMode: Text.WordWrap
        font.pixelSize: 10
        color: "#888888"
        text: move.note
    }
}
