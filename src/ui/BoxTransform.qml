import QtQuick

/*
 * UI part: BoxTransform; contract: docs/ui/box-manipulation-plan.md#shared-
 * interaction-and-geometry
 *
 * The ONE implementation of box geometry and hit testing that the Theme
 * Editor's placement overlay and the Edit canvas both consume (task BX.1).
 * Pure functions over the authored 1920x1080 canvas and the preview's zoom,
 * so the two pages cannot keep two ideas of where a corner is, which edge a
 * drag moves, or when an axis snaps to the slide centre.
 *
 * The resize is the correction the review named: a corner drag moves BOTH
 * edges that corner owns and fixes the opposite corner. The previous code in
 * both pages computed only the dragged left/top edge, so bottom-right was
 * inert and the other two right-hand corners moved one axis. Each moving edge
 * is clamped against its fixed opposite (the 20-authored-unit gesture
 * minimum) and the canvas, so a drag can never flip the box or change its
 * stack order.
 */
QtObject {
    id: transform

    readonly property real canvasWidth: 1920
    readonly property real canvasHeight: 1080
    // Below 20 authored units a box cannot be grabbed back by its own
    // handles; the gesture floor is the contract's existing number.
    readonly property real minimumSize: 20
    // The visible corner mark is 8 logical px; the hit area around it is at
    // least this radius and is compared in SCREEN pixels, so aiming feels
    // the same at every window size and zoom.
    readonly property real handleRadius: 8

    function clamp(value, lo, hi) {
        return Math.max(lo, Math.min(hi, value))
    }

    // Keep the size, pull either axis onto the 1920x1080 centre when it is
    // within `tolerance` authored units. `tolerance` is converted from
    // screen pixels by the caller, so it stays screen-relative.
    function snapCentre(x, y, w, h, tolerance) {
        let sx = x
        let sy = y
        if (Math.abs((x + w / 2) - canvasWidth / 2) <= tolerance)
            sx = canvasWidth / 2 - w / 2
        if (Math.abs((y + h / 2) - canvasHeight / 2) <= tolerance)
            sy = canvasHeight / 2 - h / 2
        return Qt.rect(sx, sy, w, h)
    }

    // Is (px, py) over this box's body? Screen pixels; the caller owns what
    // a body press means (Edit selects the topmost box, the overlay moves
    // the view's own rectangle).
    function inside(box, px, py, zoom) {
        const left = box.x * zoom
        const top = box.y * zoom
        return px >= left && px <= (box.x + box.width) * zoom
                && py >= top && py <= (box.y + box.height) * zoom
    }

    // The corner under the pointer, or "". Every corner within the radius is
    // a candidate; when a tiny box makes two overlap, the NEAREST centre wins
    // deterministically — never whichever happened to be tested first.
    function cornerAt(box, px, py, zoom, radius) {
        const r = radius === undefined ? handleRadius : radius
        const corners = [
            {mode: "tl", x: box.x * zoom, y: box.y * zoom},
            {mode: "tr", x: (box.x + box.width) * zoom, y: box.y * zoom},
            {mode: "bl", x: box.x * zoom, y: (box.y + box.height) * zoom},
            {mode: "br", x: (box.x + box.width) * zoom, y: (box.y + box.height) * zoom}
        ]
        let best = ""
        let bestDistance = Infinity
        for (let i = 0; i < corners.length; ++i) {
            const c = corners[i]
            const dx = px - c.x
            const dy = py - c.y
            if (Math.abs(dx) > r || Math.abs(dy) > r)
                continue
            const distance = dx * dx + dy * dy
            if (distance < bestDistance) {
                bestDistance = distance
                best = c.mode
            }
        }
        return best
    }

    // Body move: preserve the captured size and clamp the rectangle inside
    // the canvas. An off-canvas legacy box is corrected on the first drag
    // rather than normalised merely by being selected.
    function move(start, dx, dy) {
        const x = clamp(start.x + dx, 0, Math.max(0, canvasWidth - start.width))
        const y = clamp(start.y + dy, 0, Math.max(0, canvasHeight - start.height))
        return Qt.rect(x, y, start.width, start.height)
    }

    // Corner resize: move the two edges the corner owns, fix the opposite
    // corner, and clamp each moving edge against that fixed edge (minimum
    // size) and the canvas. BR therefore changes BOTH dimensions; TR moves
    // the right and top edges; BL the left and bottom; TL the left and top.
    function resize(mode, start, dx, dy, minimum) {
        const min = minimum === undefined ? minimumSize : minimum
        let left = start.x
        let top = start.y
        let right = start.x + start.width
        let bottom = start.y + start.height
        if (mode === "tl" || mode === "bl")
            left = clamp(start.x + dx, 0, right - min)
        if (mode === "tr" || mode === "br")
            right = clamp(start.x + start.width + dx, left + min, canvasWidth)
        if (mode === "tl" || mode === "tr")
            top = clamp(start.y + dy, 0, bottom - min)
        if (mode === "bl" || mode === "br")
            bottom = clamp(start.y + start.height + dy, top + min, canvasHeight)
        return Qt.rect(left, top, right - left, bottom - top)
    }

    // The pointer the operator should see over a corner or the body.
    function cursorFor(mode) {
        if (mode === "move")
            return Qt.SizeAllCursor
        if (mode === "tl" || mode === "br")
            return Qt.SizeFDiagCursor
        if (mode === "tr" || mode === "bl")
            return Qt.SizeBDiagCursor
        return Qt.ArrowCursor
    }
}
