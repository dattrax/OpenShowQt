import QtQuick
import QtQuick.Controls

/*
 * The edit page stage (task 4.3, docs/reference.md §7): the slide's text
 * boxes on a 16:9 canvas of 1920x1080 authored units, the view scaled
 * by width/1920 like the reference's. Since task 6.17a2 a slide is a
 * STACK of boxes, and this canvas is the chrome over that stack: one
 * outline per box in draw order, handles on the SELECTED box only, a
 * press that selects the topmost box under the pointer, drag to move and
 * a corner to resize — either commits once on release, one undo entry per
 * gesture. A move gesture whose box centre comes within a few screen
 * pixels of the canvas centre snaps that axis exactly onto it and draws a
 * guide line (review 2026-11-10), the same rule ThemeEditorPage's
 * placement layer uses.
 *
 * What is inside the boxes is the worker's own render (task 6.16a): the
 * slide is prepared and drawn into the preview stream's off-air target,
 * so the text here wraps, fits and falls back exactly as the audience's
 * will. The canvas contributes the geometry it is editing and the chrome
 * around it, never a second layout of the words.
 *
 * Which box is selected lives in the controller, not here: the canvas
 * highlights docs.currentBox and every gesture addresses it, so the
 * outlined box, the inspector's words and the edited geometry cannot be
 * three different boxes. Escape and a press on empty canvas drop the
 * handles (a chrome state, local); a press on a box selects it (a session
 * state, the controller's).
 */
Item {
    id: canvas
    objectName: "editCanvas"

    // Handles shown on the selected box ("" between gestures hides them
    // without moving the selection).
    property bool selected: true
    readonly property bool hasSlide: docs.currentSlide >= 0
        && docs.slides.length > docs.currentSlide

    // EW.5: in-place text editing. The mode is this canvas's, the session is
    // InlineTextEditor's, and the selection it edits is the SELECTED box —
    // one box, one draft, one document identity. `textEditAvailable` is the
    // honest gate: a text box under the selection and a worker whose hello
    // offers the geometry answer. Without either, Edit keeps its explicit
    // Source text drawer.
    property bool textMode: false
    property string textSlideId: ""
    property string textBoxId: ""
    // The caret each box remembered when the operator last left it (EW.5's
    // "resume the remembered caret, else the end"). Keyed by slide/box id, so
    // a reordered slide cannot hand its caret to another box.
    property var caretMemory: ({})
    readonly property bool textEditAvailable: worker.textEditorSupported
        && hasSlide && currentBox >= 0 && docs.currentBoxKind === "text"

    function beginTextEditing(offset) {
        if (!textEditAvailable)
            return false
        const box = canvas.selectedBox()
        const slideId = String(docs.slides[docs.currentSlide].id)
        if (box === undefined)
            return false
        const memoryKey = slideId + "/" + String(box.id)
        const resume = offset >= 0 ? offset
            : (canvas.caretMemory[memoryKey] !== undefined
               ? canvas.caretMemory[memoryKey] : -1)
        if (canvas.dragMode !== "")
            canvas.cancelDrag()
        canvas.textSlideId = slideId
        canvas.textBoxId = String(box.id)
        inlineEditor.slideId = slideId
        inlineEditor.boxId = canvas.textBoxId
        inlineEditor.itemIndex = canvas.currentBox
        inlineEditor.zoom = canvas.zoom
        inlineEditor.box = Qt.rect(canvas.boxX, canvas.boxY, canvas.boxW, canvas.boxH)
        inlineEditor.sceneJson = docs.sceneJsonForSlide(slideId)
        inlineEditor.initialCaret = resume
        canvas.selected = true
        canvas.textMode = true
        inlineEditor.active = true
        inlineEditor.openAt(resume)
        return true
    }
    function endTextEditing() {
        if (!canvas.textMode)
            return
        inlineEditor.finish()
    }
    function cancelTextEditing() {
        endTextEditing()
    }
    function textIdentityHolds() {
        if (!canvas.hasSlide)
            return false
        const box = canvas.selectedBox()
        return box !== undefined
            && String(docs.slides[docs.currentSlide].id) === canvas.textSlideId
            && String(box.id) === canvas.textBoxId
    }
    // A document or style edit while the mode is held re-asks geometry for
    // the same draft; a removed slide/box or a navigation ends the session.
    function refreshTextGeometry() {
        if (!canvas.textMode)
            return
        if (!canvas.textIdentityHolds()) {
            canvas.endTextEditing()
            return
        }
        inlineEditor.box = Qt.rect(canvas.boxX, canvas.boxY, canvas.boxW, canvas.boxH)
        inlineEditor.sceneJson = docs.sceneJsonForSlide(canvas.textSlideId)
        inlineEditor.askGeometry()
    }

    // The slide's boxes in DRAW order, straight from the model; and the
    // selected one's rectangle, in authored units, mirrored from the
    // document. Only the gestures below move it, and a commit reloads it.
    // The slide's boxes are re-read whenever the slide OR the document
    // changes: `currentSlideBoxes` depends on navigation too, and a fresh
    // slide is selected after the edit that created it (documentsChanged
    // fires first). Bumping one tick in every relevant handler keeps the
    // binding honest without a second box list.
    property int boxesTick: 0
    readonly property var boxes: { boxesTick; return docs.currentSlideBoxes }
    readonly property int currentBox: docs.currentBox
    property real boxX: 160
    property real boxY: 100
    property real boxW: 1600
    property real boxH: 880

    // BX.4: the fitted stage leaves one handle radius of room on every side,
    // so a corner at a slide edge is still inside this canvas's own bounds
    // (a child outside its parent's bounds receives no pointer events).
    readonly property real stageWidth: Math.max(
        1, Math.min(width - 2 * gestureInset, (height - 2 * gestureInset) * 16 / 9))
    readonly property real stageHeight: stageWidth * 9 / 16
    readonly property real zoom: stageWidth / 1920

    // Drag state: "" between gestures, else "move" or a corner id.
    property string dragMode: ""
    property point dragOrigin
    property rect startBox
    // Task BX.2: the target is captured by IDENTITY at press — the slide and
    // the box — never a current index that can move under a held drag. A
    // release whose identity no longer matches writes nothing.
    property string dragSlideId: ""
    property string dragBoxId: ""
    // The gesture area extends this far beyond the fitted content so the
    // outer half of every visible corner handle is reachable at slide edges.
    readonly property real gestureInset: 8
    // Centre snapping: a move gesture whose box centre comes within a few
    // SCREEN pixels of the canvas centre lands exactly on it, with a guide
    // line saying why. The tolerance is converted to authored units so it
    // feels the same at every window size.
    property bool snappedX: false
    property bool snappedY: false
    readonly property real snapTolerance: 6 / Math.max(zoom, 0.0001)

    readonly property color canvasColor: docs.currentTheme.backgroundColor || "#000000"

    // Task BX.1: geometry, hit testing and centre snapping are one shared
    // implementation (BoxTransform.qml) the Theme Editor consumes too. This
    // canvas keeps only its own body-selection rule and its document verbs.
    BoxTransform { id: transform }

    // Kept as the canvas's own spelling of the shared rule so the gesture and
    // its fixture still agree on one answer; the implementation lives in
    // BoxTransform.
    function snappedCentre(x, y, w, h, tolerance) {
        return transform.snapCentre(x, y, w, h, tolerance)
    }

    function syncFromDocuments() {
        const box = boxes[currentBox]
        if (box === undefined) {
            return  // no slide, or a slide the operator emptied of boxes
        }
        boxX = box.x
        boxY = box.y
        boxW = box.width
        boxH = box.height
    }

    // A page (re)entry shows the slide's stack; undo, redo and navigation
    // refresh it, mid-drag document churn does not yank the box out from
    // under the pointer. Completion matters most: the document loads
    // before any QML exists, so no signal announces what the window opens
    // on. Selection changes re-sync too — the handles move to a box of a
    // different size, and the X/Y/W/H fields read these same properties.
    Component.onCompleted: {
        syncFromDocuments()
        selected = true
    }
    onVisibleChanged: {
        if (visible) {
            syncFromDocuments()
            selected = true
        } else if (textMode) {
            endTextEditing()
        }
    }
    Connections {
        target: docs
        function onDocumentsChanged() {
            ++canvas.boxesTick
            if (canvas.dragMode === "") {
                canvas.syncFromDocuments()
            } else if (!canvas.dragIdentityHolds()) {
                // The slide or the box left under the held pointer: cancel
                // rather than commit the captured rectangle onto whatever
                // now occupies the selection.
                canvas.cancelDrag()
            }
            if (canvas.textMode)
                canvas.refreshTextGeometry()
        }
        function onNavigationChanged() {
            ++canvas.boxesTick
            // Moving to another slide cancels the gesture and ends the text
            // session; it never retargets. The session commits its draft
            // first, so the one undo entry names the box it was typed into.
            if (canvas.textMode)
                canvas.endTextEditing()
            canvas.cancelDrag()
            canvas.syncFromDocuments()
            canvas.selected = true
        }
        function onBoxSelectionChanged() {
            ++canvas.boxesTick
            if (canvas.textMode && !canvas.textIdentityHolds())
                canvas.endTextEditing()
            if (canvas.dragMode !== "") {
                canvas.cancelDrag()
            }
            canvas.syncFromDocuments()
        }
    }
    // Escape steps out of the selection through the key map; a held drag
    // must be discarded, not committed.
    onSelectedChanged: {
        if (!selected && canvas.textMode)
            canvas.endTextEditing()
        if (!selected && canvas.dragMode !== "")
            canvas.cancelDrag()
    }

    // Hit order mirrors the interaction and the paint order: a selected
    // handle beats its box, the TOPMOST box under the pointer beats the
    // others and beats empty canvas. docs.boxAt is the same topmost rule
    // in model coordinates — the canvas does not keep a second idea of
    // which box is on top.
    function hitTest(px, py) {
        if (!hasSlide) {
            return ""
        }
        const box = boxes[currentBox]
        if (selected && box !== undefined) {
            const corner = transform.cornerAt(box, px, py, zoom)
            if (corner !== "") {
                return corner
            }
        }
        const hit = docs.boxAt(px / zoom, py / zoom)
        return hit < 0 ? "" : "move"
    }

    // Task BX.1: the shared helper moves both edges of the grabbed corner
    // and anchors the opposite one. Move preserves the captured size; centre
    // snapping applies to moves only, with the tolerance in authored units
    // derived from the screen-relative 6 px.
    function applyDrag(dx, dy) {
        if (dragMode === "move") {
            const moved = transform.move(startBox, dx, dy)
            const snapped = transform.snapCentre(moved.x, moved.y, startBox.width,
                                                 startBox.height, snapTolerance)
            boxX = snapped.x
            boxY = snapped.y
            snappedX = Math.abs((boxX + startBox.width / 2) - 960) < 0.5
            snappedY = Math.abs((boxY + startBox.height / 2) - 540) < 0.5
            return
        }
        snappedX = false
        snappedY = false
        const resized = transform.resize(dragMode, startBox, dx, dy)
        boxX = resized.x
        boxY = resized.y
        boxW = resized.width
        boxH = resized.height
    }

    // The box the current selection names, or undefined when there is none.
    function selectedBox() {
        return canvas.boxes[canvas.currentBox]
    }

    function captureDragIdentity() {
        canvas.dragSlideId = canvas.hasSlide
                ? String(docs.slides[docs.currentSlide].id) : ""
        const box = canvas.selectedBox()
        canvas.dragBoxId = box === undefined ? "" : String(box.id)
    }

    function dragIdentityHolds() {
        if (!canvas.hasSlide)
            return false
        const slideId = String(docs.slides[docs.currentSlide].id)
        const box = canvas.selectedBox()
        return slideId === canvas.dragSlideId && box !== undefined
                && String(box.id) === canvas.dragBoxId
    }

    // BX.2: a completed gesture writes ONE rectangle through the box's own
    // verb — unless the identity moved (navigation, a new selection, a
    // deletion) or the pointer did not change the rounded rectangle.
    function endDrag() {
        if (canvas.dragMode === "")
            return
        const holds = canvas.dragIdentityHolds()
        const x = Math.round(canvas.boxX)
        const y = Math.round(canvas.boxY)
        const w = Math.round(canvas.boxW)
        const h = Math.round(canvas.boxH)
        const unchanged = x === Math.round(canvas.startBox.x)
                && y === Math.round(canvas.startBox.y)
                && w === Math.round(canvas.startBox.width)
                && h === Math.round(canvas.startBox.height)
        canvas.dragMode = ""
        canvas.snappedX = false
        canvas.snappedY = false
        if (!holds || unchanged) {
            canvas.syncFromDocuments()
            return
        }
        docs.setTextBoxGeometry(x, y, w, h)
    }

    // Cancel: no write, and the outline returns to the model's rectangle.
    function cancelDrag() {
        if (canvas.dragMode === "")
            return
        canvas.dragMode = ""
        canvas.snappedX = false
        canvas.snappedY = false
        canvas.syncFromDocuments()
    }

    // BX.3: the box menu is where placement reset lives now that the numeric
    // geometry band is gone. It opens on right-click and on Menu/Shift+F10,
    // and its one action uses the same domain verb the old Follow-theme
    // button did. It is enabled only while the slide holds an own placement,
    // so merely opening it can never materialise one.
    function openBoxMenuAt(px, py) {
        if (!canvas.hasSlide || canvas.currentBox < 0)
            return
        const point = canvas.mapFromItem(stage, px, py)
        boxMenu.popup(point.x, point.y)
    }
    function openBoxMenuAtSelection() {
        const box = canvas.selectedBox()
        if (box === undefined)
            return
        canvas.openBoxMenuAt((canvas.boxX + canvas.boxW / 2) * canvas.zoom,
                             (canvas.boxY + canvas.boxH / 2) * canvas.zoom)
    }

    Menu {
        id: boxMenu
        objectName: "editBoxMenu"
        MenuItem {
            objectName: "editTextMenuItem"
            text: "Edit text"
            enabled: canvas.textEditAvailable
            onClicked: canvas.beginTextEditing(-1)
        }
        MenuItem {
            objectName: "resetBoxPlacementMenuItem"
            text: "Reset box placement"
            enabled: {
                const entry = docs.slideAppearance["placement"]
                return entry !== undefined && entry.customized === true
            }
            onClicked: docs.revertCurrentSlideField("placement")
        }
    }

    // Blank canvas and other boxes do not necessarily take keyboard focus.
    // End text mode explicitly when a press misses the box being edited.
    MouseArea {
        objectName: "editTextOutsideClick"
        anchors.fill: parent
        visible: canvas.textMode
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onPressed: function (mouse) {
            const point = stage.mapFromItem(this, mouse.x, mouse.y)
            canvas.endTextEditing()
            const hit = docs.boxAt(point.x / canvas.zoom, point.y / canvas.zoom)
            canvas.selected = hit >= 0
            if (hit >= 0) {
                docs.selectTextBox(hit)
                canvas.syncFromDocuments()
            }
        }
    }

    // The stage (task CS.2): a SlidePreviewSurface on the Selection target —
    // the worker's off-air render of THIS slide, letterboxed honestly — with
    // the box chrome riding the fitted content rectangle the pixels occupy.
    // The surface never asks for a scene: preparing this slide stays the
    // page's and the controller's business.
    SlidePreviewSurface {
        id: stage
        x: (canvas.width - canvas.stageWidth) / 2
        y: (canvas.height - canvas.stageHeight) / 2
        width: canvas.stageWidth
        height: canvas.stageHeight
        controller: worker
        target: "selection"
        frameColor: canvas.canvasColor
        borderWidth: 0
        pixelsVisible: canvas.hasSlide && worker.selectionReady
        // What the emptiness means, in one honest sentence per case — the
        // worker's inability, an in-flight render, an empty slide — never a
        // Qt layout of the words standing in for the render (review R16).
        placeholder: !canvas.hasSlide
                     ? "Select or create a slide to edit its text boxes"
                     : (canvas.currentBox < 0
                        ? "this slide has no text boxes — use \"New box\" below"
                        : (!worker.selectionReady
                           ? (worker.selectionSupported
                              ? "waiting for the worker's render of this slide…"
                              : "this worker cannot render a slide that is not on air")
                           : ""))
        placeholderColor: "#929292"
        placeholderPixelSize: 13

        // One outline per box, in the model's draw order — every box the
        // worker paints is framed here, and the selected one wears the
        // accent border and the handles. The selected box's outline follows
        // the live drag (boxX.. are what the gesture moves); the others
        // sit where the model says, because nothing else can move them.
        Repeater {
            model: canvas.hasSlide ? canvas.boxes : []
            delegate: Rectangle {
                required property var modelData
                required property int index
                readonly property bool liveDrag: index === canvas.currentBox
                                                 && canvas.dragMode !== ""
                visible: canvas.hasSlide
                x: (liveDrag ? canvas.boxX : modelData.x) * canvas.zoom
                y: (liveDrag ? canvas.boxY : modelData.y) * canvas.zoom
                width: (liveDrag ? canvas.boxW : modelData.width) * canvas.zoom
                height: (liveDrag ? canvas.boxH : modelData.height) * canvas.zoom
                color: "transparent"
                border.width: index === canvas.currentBox && canvas.selected ? 2 : 1
                border.color: index === canvas.currentBox && canvas.selected
                              ? "#1685f8" : "#b0b0b0"
            }
        }

        Repeater {
            model: canvas.selected && canvas.hasSlide && canvas.currentBox >= 0
                && !canvas.textMode
                ? [Qt.point(0, 0), Qt.point(1, 0), Qt.point(0, 1), Qt.point(1, 1)]
                : []
            delegate: Rectangle {
                required property var modelData
                width: 8
                height: 8
                color: "#1685f8"
                border.color: "#202020"
                x: canvas.boxX * canvas.zoom
                    + (modelData.x === 0 ? -width / 2
                                         : canvas.boxW * canvas.zoom - width / 2)
                y: canvas.boxY * canvas.zoom
                    + (modelData.y === 0 ? -height / 2
                                         : canvas.boxH * canvas.zoom - height / 2)
            }
        }

        // The centre guides: one line per axis that the gesture has
        // snapped, so the operator sees the reason the box stopped moving.
        Rectangle {
            objectName: "centreGuideX"
            visible: canvas.dragMode === "move" && canvas.snappedX
            // These are children of the fitted content overlay, whose
            // origin already includes the stage's letterbox offset.
            x: parent.width / 2
            y: 0
            width: 1
            height: parent.height
            color: "#ffd666"
            z: 5
        }
        Rectangle {
            objectName: "centreGuideY"
            visible: canvas.dragMode === "move" && canvas.snappedY
            x: 0
            y: parent.height / 2
            width: parent.width
            height: 1
            color: "#ffd666"
            z: 5
        }

        // The gesture surface covers the fitted content AND the outer half
        // of every handle (task BX.2): with the content at a slide edge the
        // visible corner extends beyond it, so an exact-fill area would make
        // that half dead. Coordinates are mapped back by `gestureInset`
        // before hit testing; drag deltas are differences and cancel it.
        MouseArea {
            id: gestureArea
            objectName: "editGestureArea"
            anchors.fill: parent
            anchors.margins: -canvas.gestureInset
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            // Edit lives inside a Flickable. Once a box/handle accepts a
            // press, keep its grab through the whole drag instead of
            // allowing the page to steal it and cancel the edit.
            preventStealing: true
            hoverEnabled: true
            cursorShape: canvas.dragMode !== ""
                         ? transform.cursorFor(canvas.dragMode)
                         : transform.cursorFor(
                               canvas.hitTest(mouseX - canvas.gestureInset,
                                              mouseY - canvas.gestureInset))
            // Focus so the canvas's own Escape (below) reaches the item
            // scope rather than the window's clear-all shortcut.
            focus: true
            // EW.5: text mode owns the pointer over its box; the box gesture
            // surface stands down rather than competing for a click.
            enabled: !canvas.textMode

            onDoubleClicked: function (mouse) {
                const px = mouse.x - canvas.gestureInset
                const py = mouse.y - canvas.gestureInset
                if (!canvas.hasSlide)
                    return
                const hit = docs.boxAt(px / canvas.zoom, py / canvas.zoom)
                if (hit < 0)
                    return
                const box = canvas.boxes[hit]
                if (box === undefined || box.kind !== "text")
                    return
                docs.selectTextBox(hit)
                canvas.syncFromDocuments()
                canvas.selected = true
                canvas.beginTextEditing(-1)
            }

            onPressed: function (mouse) {
                const px = mouse.x - canvas.gestureInset
                const py = mouse.y - canvas.gestureInset
                if (mouse.button === Qt.RightButton) {
                    // A right-click selects the box under the pointer (the
                    // topmost body rule) and opens the box menu; it never
                    // starts a drag.
                    if (canvas.hasSlide) {
                        const hit = docs.boxAt(px / canvas.zoom, py / canvas.zoom)
                        if (hit >= 0) {
                            docs.selectTextBox(hit)
                            canvas.syncFromDocuments()
                            canvas.selected = true
                        }
                    }
                    const point = canvas.mapFromItem(gestureArea, mouse.x, mouse.y)
                    boxMenu.popup(point.x, point.y)
                    return
                }
                const mode = canvas.hitTest(px, py)
                if (mode === "") {
                    canvas.selected = false
                    mouse.accepted = false
                    return
                }
                // Pressing a box BODY works on THAT box: the topmost one
                // under the pointer becomes the session's selection before
                // the gesture starts, so the drag, the fields and the
                // inspector all follow what was clicked. A HANDLE press
                // never re-selects — a selected box's corner can lie on
                // another box, and the resize below is anchored to the box
                // whose corner was grabbed, so silently moving the
                // selection would commit that anchor's geometry onto the
                // wrong box. Pressing the already-selected box re-selects
                // it; the controller no-ops that without dirtying anything.
                if (mode === "move") {
                    const hit = docs.boxAt(px / canvas.zoom, py / canvas.zoom)
                    if (hit >= 0) {
                        docs.selectTextBox(hit)
                        // startBox must be the box just clicked, not the one
                        // the properties still hold from before the press.
                        canvas.syncFromDocuments()
                    }
                }
                canvas.selected = true
                canvas.dragMode = mode
                canvas.dragOrigin = Qt.point(mouse.x, mouse.y)
                canvas.startBox = Qt.rect(canvas.boxX, canvas.boxY,
                                          canvas.boxW, canvas.boxH)
                canvas.captureDragIdentity()
                canvas.snappedX = false
                canvas.snappedY = false
            }
            onPositionChanged: function (mouse) {
                if (canvas.dragMode === "" || !canvas.hasSlide) {
                    return
                }
                canvas.applyDrag((mouse.x - canvas.dragOrigin.x) / canvas.zoom,
                                 (mouse.y - canvas.dragOrigin.y) / canvas.zoom)
            }
            onReleased: function (mouse) {
                if (mouse.button === Qt.LeftButton)
                    canvas.endDrag()
            }
            onCanceled: canvas.cancelDrag()
            Keys.onMenuPressed: canvas.openBoxMenuAtSelection()
            Keys.onPressed: function (event) {
                if (event.key === Qt.Key_F10 && (event.modifiers & Qt.ShiftModifier)) {
                    canvas.openBoxMenuAtSelection()
                    event.accepted = true
                    return
                }
                // Enter on a selected text box enters the mode (EW.5); F2
                // keeps its output meaning and is never a second owner here.
                if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                    && !event.modifiers && canvas.textEditAvailable) {
                    canvas.beginTextEditing(-1)
                    event.accepted = true
                }
            }
        }

        // EW.4/EW.5: the native input bridge and editing chrome. It rides
        // the fitted stage, so its authored-unit geometry and this canvas's
        // zoom are the only two transforms between the worker's paragraph
        // and what the operator sees.
        InlineTextEditor {
            id: inlineEditor
            anchors.fill: parent
            onFinished: function (restoreCanvasFocus) {
                if (canvas.textSlideId !== "" && canvas.textBoxId !== "") {
                    canvas.caretMemory[canvas.textSlideId + "/" + canvas.textBoxId] =
                        inlineEditor.caretOffset
                }
                canvas.textMode = false
                canvas.syncFromDocuments()
                canvas.selected = true
                if (restoreCanvasFocus) gestureArea.forceActiveFocus()
            }
            onGeometryLost: {
                // No matching layout: keep the committed draft, clear the
                // chrome, and hand the operator the explicit source drawer.
                if (!worker.textEditorSupported)
                    canvas.endTextEditing()
            }
        }
    }
}
