import QtQuick

/*
 * The edit page stage (task 4.3, docs/reference.md §7): the slide's text
 * box drawn on a 16:9 canvas of 1920x1080 authored units, the view scaled
 * by width/1920 like the reference's. Drag inside the box moves it, a
 * corner handle resizes it around the opposite corner, and either
 * commits once on release — one undo entry per gesture, the reference's
 * rule. The text inside is a representation, not the render: wrapped at
 * the box width and shrunk when it overflows, so it stands where the
 * worker will paint while the true appearance stays on the preview.
 *
 * With one item per slide, arriving on the page selects it; a click on
 * empty canvas, or an Escape (which Main hands to the selection before
 * the output clear-all can see it), steps back.
 */
Item {
    id: canvas

    property bool selected: true
    readonly property bool hasSlide: docs.currentSlide >= 0
        && docs.slides.length > docs.currentSlide

    // The box under edit, in authored units. Synced from the document;
    // only the gestures below move it, and a commit reloads it.
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
        if (!hasSlide) {
            return
        }
        const row = docs.slides[docs.currentSlide]
        boxX = row.x
        boxY = row.y
        boxW = row.width
        boxH = row.height
    }

    // A page (re)entry selects the slide's item and shows its box; undo,
    // redo and navigation refresh it, mid-drag document churn does not
    // yank the box out from under the pointer. Completion matters most:
    // the document loads before any QML exists, so no signal announces
    // what the window opens on.
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
    }

    // Hit order mirrors the interaction: a selected handle beats the
    // box body, the box body beats empty canvas.
    function hitTest(px, py) {
        if (!hasSlide) {
            return ""
        }
        const tol = 8
        const left = boxX * zoom
        const top = boxY * zoom
        const right = (boxX + boxW) * zoom
        const bottom = (boxY + boxH) * zoom
        if (selected) {
            if (Math.abs(px - left) <= tol && Math.abs(py - top) <= tol) return "tl"
            if (Math.abs(px - right) <= tol && Math.abs(py - top) <= tol) return "tr"
            if (Math.abs(px - left) <= tol && Math.abs(py - bottom) <= tol) return "bl"
            if (Math.abs(px - right) <= tol && Math.abs(py - bottom) <= tol) return "br"
        }
        if (px >= left && px <= right && py >= top && py <= bottom) return "move"
        return ""
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
        border.color: "#2f3542"

        // The text representation (task 4.4): the resolved style's font,
        // colour, alignment and line box, shrunk to fit the way the
        // worker's layout does. The size cap is the style's own when it
        // sets one — this is a representation, not the render, and the
        // true picture stays on the preview.
        Text {
            visible: canvas.hasSlide
            x: canvas.boxX * canvas.zoom
            y: canvas.boxY * canvas.zoom
            width: canvas.boxW * canvas.zoom
            height: canvas.boxH * canvas.zoom
            horizontalAlignment: docs.currentStyle.align === "left" ? Text.AlignLeft
                : docs.currentStyle.align === "right" ? Text.AlignRight
                : Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            wrapMode: Text.WordWrap
            fontSizeMode: Text.Fit
            font.family: docs.currentStyle.fontFamily || ""
            font.pixelSize: Math.round((docs.currentStyle.fontSize > 0
                                        ? docs.currentStyle.fontSize : 96) * canvas.zoom)
            lineHeight: docs.currentStyle.lineSpacing || 1.25
            lineHeightMode: Text.ProportionalHeight
            minimumPixelSize: 8
            color: docs.currentStyle.textColor || "#f0f0ff"
            text: docs.currentSlideText
        }

        // The box outline: quiet when unselected, accent-coloured and
        // two pixels wide when it is the thing under the pointer.
        Rectangle {
            id: overlay
            visible: canvas.hasSlide
            x: canvas.boxX * canvas.zoom
            y: canvas.boxY * canvas.zoom
            width: canvas.boxW * canvas.zoom
            height: canvas.boxH * canvas.zoom
            color: "transparent"
            border.width: canvas.selected ? 2 : 1
            border.color: canvas.selected ? "#f0008c" : "#8f93a5"
        }

        Repeater {
            model: canvas.selected && canvas.hasSlide
                ? [Qt.point(0, 0), Qt.point(1, 0), Qt.point(0, 1), Qt.point(1, 1)]
                : []
            delegate: Rectangle {
                required property var modelData
                width: 8
                height: 8
                color: "#f0008c"
                border.color: "#191923"
                x: overlay.x + (modelData.x === 0 ? -width / 2 : overlay.width - width / 2)
                y: overlay.y + (modelData.y === 0 ? -height / 2 : overlay.height - height / 2)
            }
        }

        Text {
            anchors.centerIn: parent
            visible: !canvas.hasSlide
            color: "#6a6f80"
            font.pixelSize: 13
            text: "Select or create a slide to edit its text box"
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
                docs.setSlideGeometry(Math.round(canvas.boxX), Math.round(canvas.boxY),
                                      Math.round(canvas.boxW), Math.round(canvas.boxH))
            }
        }
    }
}
