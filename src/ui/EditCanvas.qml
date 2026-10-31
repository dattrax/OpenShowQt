import QtQuick

/*
 * The edit page stage (task 4.3, docs/reference.md §7): the slide's text
 * boxes on a 16:9 canvas of 1920x1080 authored units, the view scaled
 * by width/1920 like the reference's. Since task 6.17a2 a slide is a
 * STACK of boxes, and this canvas is the chrome over that stack: one
 * outline per box in draw order, handles on the SELECTED box only, a
 * press that selects the topmost box under the pointer, drag to move and
 * a corner to resize — either commits once on release, one undo entry per
 * gesture.
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

    // Handles shown on the selected box ("" between gestures hides them
    // without moving the selection).
    property bool selected: true
    readonly property bool hasSlide: docs.currentSlide >= 0
        && docs.slides.length > docs.currentSlide

    // The slide's boxes in DRAW order, straight from the model; and the
    // selected one's rectangle, in authored units, mirrored from the
    // document. Only the gestures below move it, and a commit reloads it.
    readonly property var boxes: docs.currentSlideBoxes
    readonly property int currentBox: docs.currentBox
    property real boxX: 160
    property real boxY: 100
    property real boxW: 1600
    property real boxH: 880

    readonly property real stageWidth: Math.min(width, height * 16 / 9)
    readonly property real stageHeight: stageWidth * 9 / 16
    readonly property real zoom: stageWidth / 1920

    // Drag state: "" between gestures, else "move" or a corner id.
    property string dragMode: ""
    property point dragOrigin
    property rect startBox

    readonly property color canvasColor: docs.currentStyle.backgroundColor || "#000000"

    function clamp(v, lo, hi) {
        return Math.max(lo, Math.min(hi, v))
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
    onVisibleChanged: if (visible) {
        syncFromDocuments()
        selected = true
    }
    Connections {
        target: docs
        function onDocumentsChanged() {
            if (canvas.dragMode === "") {
                canvas.syncFromDocuments()
            }
        }
        function onNavigationChanged() {
            canvas.syncFromDocuments()
            canvas.selected = true
        }
        function onBoxSelectionChanged() {
            if (canvas.dragMode === "") {
                canvas.syncFromDocuments()
            }
        }
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
        const tol = 8
        const box = boxes[currentBox]
        if (selected && box !== undefined) {
            const left = box.x * zoom
            const top = box.y * zoom
            const right = (box.x + box.width) * zoom
            const bottom = (box.y + box.height) * zoom
            if (Math.abs(px - left) <= tol && Math.abs(py - top) <= tol) return "tl"
            if (Math.abs(px - right) <= tol && Math.abs(py - top) <= tol) return "tr"
            if (Math.abs(px - left) <= tol && Math.abs(py - bottom) <= tol) return "bl"
            if (Math.abs(px - right) <= tol && Math.abs(py - bottom) <= tol) return "br"
        }
        const hit = docs.boxAt(px / zoom, py / zoom)
        return hit < 0 ? "" : "move"
    }

    // The opposite corner stays anchored; 20 authored px is the floor —
    // below it the box cannot be grabbed back by its own handles.
    function applyDrag(dx, dy) {
        if (dragMode === "move") {
            boxX = clamp(startBox.x + dx, 0, 1920 - startBox.width)
            boxY = clamp(startBox.y + dy, 0, 1080 - startBox.height)
            return
        }
        const min = 20
        const right = startBox.x + startBox.width
        const bottom = startBox.y + startBox.height
        const x0 = (dragMode === "tl" || dragMode === "bl")
            ? clamp(startBox.x + dx, 0, right - min) : startBox.x
        const y0 = (dragMode === "tl" || dragMode === "tr")
            ? clamp(startBox.y + dy, 0, bottom - min) : startBox.y
        boxX = x0
        boxY = y0
        boxW = right - x0
        boxH = bottom - y0
    }

    Rectangle {
        id: stage
        x: (canvas.width - canvas.stageWidth) / 2
        y: (canvas.height - canvas.stageHeight) / 2
        width: canvas.stageWidth
        height: canvas.stageHeight
        color: canvas.canvasColor
        border.color: "#4a4a4a"

        // The slide as the WORKER draws it, rendered off the air (task 6.16a,
        // review R16). This replaced a QML Text that laid the same slide out
        // a second time — Qt's wrapping, Qt's fit, Qt's font fallbacks
        // against Skia's — so the operator composed one picture and the
        // audience saw another. Now the text, the background image and the
        // media under the boxes are the production render of the WHOLE
        // stack, and the only thing this canvas paints over them is the
        // chrome below: the outlines and the handles.
        PreviewView {
            id: workerPicture
            anchors.fill: parent
            controller: worker
            target: PreviewView.Selection
            visible: canvas.hasSlide && worker.selectionReady
        }

        // No picture yet (a first ask still in flight, a worker that cannot
        // draw an off-air scene at all): say which, rather than approximate
        // the slide. Any Qt layout of the text here would put the two
        // renderers back exactly where the review found them.
        Text {
            anchors.centerIn: parent
            visible: canvas.hasSlide && !worker.selectionReady
            color: "#929292"
            font.pixelSize: 13
            horizontalAlignment: Text.AlignHCenter
            text: worker.selectionSupported
                  ? "waiting for the worker's render of this slide…"
                  : "this worker cannot render a slide that is not on air"
        }

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

        Text {
            anchors.centerIn: parent
            visible: !canvas.hasSlide || canvas.currentBox < 0
            color: "#929292"
            font.pixelSize: 13
            text: !canvas.hasSlide
                  ? "Select or create a slide to edit its text boxes"
                  : "this slide has no text boxes — use \"New box\" below"
        }

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton
            // Focus so the canvas's own Escape (below) reaches the item
            // scope rather than the window's clear-all shortcut.
            focus: true

            onPressed: function (mouse) {
                const mode = canvas.hitTest(mouse.x, mouse.y)
                if (mode === "") {
                    canvas.selected = false
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
                    const hit = docs.boxAt(mouse.x / canvas.zoom, mouse.y / canvas.zoom)
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
            }
            onPositionChanged: function (mouse) {
                if (canvas.dragMode === "" || !canvas.hasSlide) {
                    return
                }
                canvas.applyDrag((mouse.x - canvas.dragOrigin.x) / canvas.zoom,
                                 (mouse.y - canvas.dragOrigin.y) / canvas.zoom)
            }
            onReleased: {
                if (canvas.dragMode === "") {
                    return
                }
                canvas.dragMode = ""
                // Whole authored pixels: the same coordinates the X/Y/W/H
                // fields show, and a clean value for the history entry.
                docs.setTextBoxGeometry(Math.round(canvas.boxX), Math.round(canvas.boxY),
                                        Math.round(canvas.boxW), Math.round(canvas.boxH))
            }
        }
    }
}
