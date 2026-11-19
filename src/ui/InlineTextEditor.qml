import QtQuick
import QtQuick.Controls

/*
 * UI part: InlineTextEditor (EW.4/EW.5)
 *
 * The native input bridge for in-place text editing. The words on the stage
 * stay the worker's render; this item contributes only the input sink and
 * the editing chrome — caret, selection and IME composition — whose geometry
 * comes from the worker's own laid-out paragraph (`text.editor`), never from
 * Qt's font metrics.
 *
 * The TextInput is the bridge, not the display: its text is transparent, its
 * caret and selection are hidden, and it is sized/positioned over the
 * authored box so IME candidate windows and clipboard shortcuts land where
 * the operator is looking. Pointer selection is done here with worker hit
 * tests, so a click or drag selects exactly the glyphs under the pointer,
 * including All caps expansions.
 *
 * A draft is identity-bound to one show/slide/box by the host (EditCanvas).
 * Every keystroke updates `docs` through the controller's ordinary debounce
 * and asks for fresh geometry; replies are accepted by request id only, so a
 * late answer can never move the caret or paint another slide's chrome.
 */
Item {
    id: editor
    objectName: "inlineTextEditor"
    visible: active
    clip: false

    // Host contract (EditCanvas).
    property bool active: false
    property real zoom: 1.0
    property rect box: Qt.rect(0, 0, 1920, 1080)
    property string sceneJson: ""
    property int itemIndex: -1
    property string slideId: ""
    property string boxId: ""
    // -1 means "resume the remembered caret / end of the text".
    property int initialCaret: -1

    // Session state.
    property string draft: ""
    property bool geometryFresh: false
    property rect caretRect: Qt.rect(0, 0, 1, 1)
    property var selectionRects: []
    property real renderedFontSize: 24
    property bool compositionPending: false
    // Request correlation: 0 means no ask in flight.
    property int pendingGeometry: 0
    property int pendingHit: 0
    property bool pressPending: false
    property int selectionAnchor: -1
    property point pendingPoint: Qt.point(0, 0)
    property point pressPoint: Qt.point(0, 0)
    property point latestPoint: Qt.point(0, 0)
    property bool movedSincePress: false
    property bool pendingSelecting: false
    property bool applyingCursor: false
    // True between openAt() and the input actually taking focus; a focus-exit
    // timer left over from a previous session must not retire this one.
    property bool opening: false

    signal finished()
    signal geometryLost()

    readonly property bool caretVisible: active && geometryFresh &&
        input.selectionStart === input.selectionEnd
    readonly property string preedit: input.preeditText
    readonly property bool editing: active
    readonly property int caretOffset: input.cursorPosition

    function finish() {
        if (!active) return
        // Escape (or focus exit) during an IME composition first lets the
        // composition finish: an uncommitted preedit must not become a
        // committed edit, and the session stays until it settles.
        if (input.preeditText.length > 0) return
        // Leaving the mode keeps the committed words and flushes the debounce
        // so the one history entry names the box the burst was typed into.
        active = false
        docs.flushText()
        draft = ""
        selectionRects = []
        geometryFresh = false
        pendingGeometry = 0
        pendingHit = 0
        pressPending = false
        selectionAnchor = -1
        finished()
    }

    function openAt(offset) {
        // The draft starts from the DOCUMENT's words for this box; the host
        // captured the identity the draft belongs to.
        draft = docs.currentSlideText
        geometryFresh = false
        selectionRects = []
        caretRect = Qt.rect(0, 0, 1, 1)
        pendingGeometry = 0
        pendingHit = 0
        pressPending = false
        selectionAnchor = -1
        input.text = draft
        const position = offset >= 0
            ? Math.max(0, Math.min(offset, draft.length)) : draft.length
        applyingCursor = true
        input.select(position, position)
        applyingCursor = false
        opening = true
        askGeometry()
        Qt.callLater(function () {
            input.forceActiveFocus()
            editor.opening = false
        })
    }

    function lines() {
        return draft.split("\n")
    }

    function askGeometry() {
        if (!active) return
        if (!worker.textEditorSupported || sceneJson === "" || itemIndex < 0) {
            geometryFresh = false
            return
        }
        const ask = {
            scene: sceneJson,
            item: itemIndex,
            lines: lines(),
            caret: input.cursorPosition
        }
        if (input.selectionStart !== input.selectionEnd) {
            ask.selection = [Math.min(input.selectionStart, input.selectionEnd),
                             Math.max(input.selectionStart, input.selectionEnd)]
        }
        const id = worker.requestEditorLayout(ask)
        if (id > 0) {
            pendingGeometry = id
        } else {
            geometryFresh = false
            selectionRects = []
            geometryLost()
        }
    }

    // Pointer asks are separate so a hit reply updates the caret and THEN a
    // geometry ask paints it; the two can never be confused by arrival order.
    function sendHit() {
        if (!active) return
        const ask = {
            scene: sceneJson,
            item: itemIndex,
            lines: lines(),
            caret: input.cursorPosition,
            point: { x: pendingPoint.x, y: pendingPoint.y }
        }
        const id = worker.requestEditorLayout(ask)
        if (id > 0) pendingHit = id
    }

    // Editor-local pixels -> authored canvas units. This item sits on the
    // stage, whose origin IS the authored canvas's origin.
    //
    // A press asks where the caret goes; a move can only extend a selection
    // once that anchor is known, so a move that arrives before the press's
    // answer is remembered and replayed rather than lost.
    function hitAt(screenX, screenY, selecting) {
        const point = Qt.point(screenX / Math.max(zoom, 0.0001),
                               screenY / Math.max(zoom, 0.0001))
        latestPoint = point
        if (!selecting) {
            pressPoint = point
            movedSincePress = false
            pendingSelecting = false
            pendingPoint = point
            hitThrottle.restart()
            return
        }
        movedSincePress = true
        if (selectionAnchor < 0) return
        pendingSelecting = true
        pendingPoint = point
        hitThrottle.restart()
    }

    function insertLineBreak() {
        if (input.selectionStart !== input.selectionEnd)
            input.remove(input.selectionStart, input.selectionEnd)
        input.insert(input.cursorPosition, "\n")
    }

    function setCaret(offset) {
        applyingCursor = true
        input.select(offset, offset)
        applyingCursor = false
    }

    function setSelection(anchor, focus) {
        applyingCursor = true
        input.select(anchor, focus)
        applyingCursor = false
    }

    function installGeometry(answer) {
        const caret = answer.caret
        if (caret !== undefined && caret.height > 0) {
            // The wire's caret carries a position and a line height; its
            // width is the UI's own one-pixel rule, scaled.
            caretRect = Qt.rect(caret.x * zoom, caret.y * zoom,
                                Math.max(1, zoom), caret.height * zoom)
        }
        const rows = []
        const selection = answer.selection !== undefined ? answer.selection : []
        for (let i = 0; i < selection.length; ++i) {
            const rect = selection[i]
            rows.push(Qt.rect(rect.x * zoom, rect.y * zoom,
                              rect.width * zoom, rect.height * zoom))
        }
        selectionRects = rows
        if (answer.fontSize > 0) renderedFontSize = answer.fontSize
        geometryFresh = true
    }

    Timer {
        id: hitThrottle
        interval: 16
        repeat: false
        onTriggered: editor.sendHit()
    }

    Connections {
        target: worker
        function onEditorLayoutReady(id, answer) {
            if (!editor.active) return
            if (id === editor.pendingHit) {
                editor.pendingHit = 0
                if (answer.hit !== undefined) {
                    const offset = answer.hit.offset
                    if (editor.pressPending) {
                        editor.pressPending = false
                        editor.selectionAnchor = offset
                        editor.setCaret(offset)
                        if (editor.movedSincePress) {
                            editor.movedSincePress = false
                            editor.pendingSelecting = true
                            editor.pendingPoint = editor.latestPoint
                            hitThrottle.restart()
                        }
                    } else if (editor.pendingSelecting && editor.selectionAnchor >= 0) {
                        editor.setSelection(editor.selectionAnchor, offset)
                    }
                }
                editor.askGeometry()
                return
            }
            if (id === editor.pendingGeometry) {
                editor.pendingGeometry = 0
                editor.installGeometry(answer)
            }
        }
        function onEditorLayoutFailed(id, reason) {
            if (!editor.active) return
            if (id !== editor.pendingGeometry && id !== editor.pendingHit) return
            editor.pendingGeometry = 0
            editor.pendingHit = 0
            editor.geometryFresh = false
            editor.selectionRects = []
            editor.geometryLost()
        }
        function onStateChanged() {
            // A worker that stopped offering the geometry while a session is
            // held: keep the draft (the document already has it), clear the
            // stale chrome and let the host expose the source fallback.
            if (!worker.textEditorSupported && editor.active) {
                editor.geometryFresh = false
                editor.selectionRects = []
                editor.geometryLost()
            }
        }
    }

    // The real input sink. Transparent by design: only the worker's pixels
    // are the words; this holds focus, IME, clipboard and cursor state.
    TextInput {
        id: input
        objectName: "inlineTextInput"
        x: editor.box.x * editor.zoom
        y: editor.box.y * editor.zoom
        width: Math.max(1, editor.box.width * editor.zoom)
        height: Math.max(1, editor.box.height * editor.zoom)
        visible: editor.active
        focus: false
        activeFocusOnPress: false
        selectByMouse: false
        color: "transparent"
        selectionColor: "transparent"
        selectedTextColor: "transparent"
        cursorVisible: false
        wrapMode: TextInput.Wrap
        inputMethodHints: Qt.ImhMultiLine
        font.pixelSize: Math.max(8, editor.renderedFontSize * editor.zoom)
        text: editor.draft
        onTextChanged: {
            if (!editor.active) return
            editor.compositionPending = preeditText.length > 0
            if (text === editor.draft) return
            editor.draft = text
            docs.setSlideText(text)
            editor.askGeometry()
        }
        onCursorPositionChanged: {
            if (editor.active && !editor.applyingCursor) editor.askGeometry()
        }
        onSelectionStartChanged: {
            if (editor.active && !editor.applyingCursor) editor.askGeometry()
        }
        onSelectionEndChanged: {
            if (editor.active && !editor.applyingCursor) editor.askGeometry()
        }
        Keys.onPressed: function (event) {
            if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                && preeditText.length === 0) {
                // Enter is an authored line break; the draft and the worker's
                // layout both see it immediately.
                editor.insertLineBreak()
                event.accepted = true
                return
            }
            // One document history owner: the native field's own undo stack
            // must never disagree with the document's. Undo/redo commit the
            // burst and then act on the document, which is exactly the one
            // granularity the modal promises.
            if (event.matches(StandardKey.Undo)) {
                editor.finish()
                docs.undo()
                event.accepted = true
            } else if (event.matches(StandardKey.Redo)) {
                editor.finish()
                docs.redo()
                event.accepted = true
            }
        }
        // Keystrokes here are typing; the window's gates already stand down
        // for a focused TextInput (typingIntoField).
        onActiveFocusChanged: {
            // The explicit focus-exit path: clicking another control (the
            // rail, an inspector field, a button) leaves the mode and keeps
            // the committed words. A one-tick delay lets the incoming focus
            // settle instead of treating a transient change as an exit.
            if (editor.active && !editor.opening && !activeFocus) focusExit.restart()
        }
    }

    Timer {
        id: focusExit
        interval: 0
        repeat: false
        onTriggered: if (editor.active && !editor.opening && !input.activeFocus) editor.finish()
    }

    // Pointer selection, driven by the worker's own hit test. The TextInput's
    // mouse selection is off so the two can never disagree about a boundary.
    MouseArea {
        id: pointer
        x: input.x
        y: input.y
        width: input.width
        height: input.height
        visible: editor.active
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        preventStealing: true
        cursorShape: Qt.IBeamCursor
        onPressed: function (mouse) {
            input.forceActiveFocus()
            editor.selectionAnchor = -1
            editor.pressPending = true
            editor.hitAt(pointer.x + mouse.x, pointer.y + mouse.y, false)
        }
        onDoubleClicked: function (mouse) {
            input.forceActiveFocus()
            editor.selectionAnchor = -1
            editor.pressPending = true
            editor.hitAt(pointer.x + mouse.x, pointer.y + mouse.y, false)
        }
        onPositionChanged: function (mouse) {
            if (!pressed) return
            editor.hitAt(pointer.x + mouse.x, pointer.y + mouse.y, true)
        }
        // No release handler: the press's own hit answer may still be in
        // flight when the pointer goes up, and it is what places the caret.
    }

    // The selection: the worker's paragraph range rectangles, in the accent
    // colour at a readable alpha. These ride the render, never replace it.
    Repeater {
        model: editor.selectionRects
        delegate: Rectangle {
            required property var modelData
            x: modelData.x
            y: modelData.y
            width: modelData.width
            height: modelData.height
            color: "#551685f8"
            z: 30
        }
    }

    // The caret.
    Rectangle {
        objectName: "inlineCaret"
        visible: editor.caretVisible
        x: editor.caretRect.x
        y: editor.caretRect.y
        width: Math.max(1, editor.caretRect.width)
        height: Math.max(1, editor.caretRect.height)
        color: "#ffffff"
        z: 31
    }

    // IME composition decoration: the candidate text is UI chrome, drawn at
    // the caret with an underline. Committed words come from the render.
    Rectangle {
        objectName: "inlineComposition"
        visible: editor.active && editor.preedit.length > 0
        x: editor.caretRect.x
        y: editor.caretRect.y
        width: compositionText.width + 2
        height: compositionText.height + 2
        color: "transparent"
        border.color: "#ffd666"
        border.width: 1
        z: 32
        Text {
            id: compositionText
            x: 1
            y: 1
            text: editor.preedit
            color: "#ffffff"
            font.pixelSize: Math.max(8, editor.renderedFontSize * editor.zoom)
        }
    }

    // The explicit Done path (the other is Escape/focus exit).
    PanelButton {
        objectName: "inlineEditorDone"
        text: "Done"
        visible: editor.active
        height: 24
        width: 60
        x: Math.min(editor.width - width, input.x + input.width - width - 2)
        y: Math.max(0, input.y - height - 2)
        z: 33
        onClicked: editor.finish()
    }
}
