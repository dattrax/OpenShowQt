import QtQuick
import QtQuick.Controls

/*
 * UI part: SlidePreviewCollection; contract: docs/ui/control-sets.md#slidepreviewcollection
 *
 * A virtualized responsive collection of SlidePreviewCards with cue/geometry
 * mapping and visible-band reporting (ungrouped mode shipped with CS.3 for
 * the Bible import; CS.7 adds the Show's grouped rows, where every
 * running-order item OCCURRENCE starts a new visual row).
 *
 * Grouped layout rules (CS.7): groups are cut by the row's occurrence
 * identity `entryId` — never by show name, theme or Bible context, so two
 * occurrences of one show are two groups — and every group starts at column
 * zero. When a group's last cue ends mid-row the rest of that row is filled
 * with INERT filler cells: they take no name, show nothing, cannot be
 * clicked, right-clicked, focused or gone; the next group's first cue lands
 * on the next row. A group that ends exactly at a row end forces no blank
 * row. Cues within a group flow left to right and wrap normally, and the
 * flat SOURCE order stays the presentation order — this view lays out, it
 * never reorders cues.
 *
 * The collection displays and maps; it never schedules scene requests.
 * Hosts keep their own flat presentation-order model and read the band
 * this view actually shows (with at most one row of look-ahead), so the
 * ask describes where the operator is looking rather than where a counter
 * happens to be.
 */
GridView {
    id: collection

    // The flat source rows (cue maps for Show, plan slides for Bible).
    // Every delegate — the host's `delegate` component — receives the
    // wrapper {cueIndex, row}: cueIndex is the row's index in THIS source,
    // row is the source entry itself.
    property var source: []
    // Grouped mode (CS.7): rows with a new entryId start on a new visual
    // row. The ungrouped flow — one uniform grid, no fillers — is what the
    // Bible import lists.
    property bool grouped: false
    // Operator zoom: the width one card asks for. With `stretchCells` the
    // view divides its own width among the columns instead, so the grid
    // fills the page edge to edge at the zoom step's target width.
    property real cardWidth: 220
    property bool stretchCells: false

    readonly property int columns: Math.max(1, Math.floor(width / cardWidth))
    cellWidth: stretchCells && width > 0 ? width / columns : cardWidth
    cellHeight: (cellWidth - 12) * 9 / 16 + 48

    /*
     * One rebuild per layout change: the visual cells and the cue->cell
     * map, as ONE recomputed value so the model and every mapping function
     * read the same generation. Reading `source`, `columns` and `grouped`
     * here is the dependency set — a resize that changes the column count
     * re-cuts every group boundary, and a source rebuild re-lays out.
     */
    readonly property var layout: {
        const src = collection.source
        const rows = (!src || src.length === undefined) ? [] : src
        const cols = Math.max(1, collection.columns)
        const cells = []
        const map = []
        if (collection.grouped) {
            let col = 0
            let prev = null
            for (let i = 0; i < rows.length; ++i) {
                const entry = rows[i].entryId
                const key = entry === undefined ? "" : String(entry)
                if (col > 0 && prev !== null && key !== prev) {
                    // The row's remaining columns become inert fill so the
                    // next occurrence starts clean at column zero.
                    while (col < cols) {
                        cells.push({filler: true})
                        ++col
                    }
                }
                map.push(cells.length)
                cells.push({cueIndex: i, row: rows[i]})
                prev = key
                col = (col + 1) % cols
            }
        } else {
            for (let i = 0; i < rows.length; ++i) {
                map.push(cells.length)
                cells.push({cueIndex: i, row: rows[i]})
            }
        }
        ({cells: cells, cellOfCue: map})
    }
    model: collection.layout.cells

    // Mapped geometry for scroll-to-cue and the ask (CS.7 contract): row
    // number and content-space rectangle for ONE cue, whatever grouping
    // era the layout is in.
    function cueRow(cueIndex) {
        const map = collection.layout.cellOfCue
        if (cueIndex < 0 || cueIndex >= map.length) return -1
        return Math.floor(map[cueIndex] / Math.max(1, collection.columns))
    }
    function cueRect(cueIndex) {
        const map = collection.layout.cellOfCue
        if (cueIndex < 0 || cueIndex >= map.length)
            return ({x: 0, y: 0, width: 0, height: 0})
        const cell = map[cueIndex]
        const cols = Math.max(1, collection.columns)
        return ({x: (cell % cols) * cellWidth,
                 y: Math.floor(cell / cols) * cellHeight,
                 width: cellWidth, height: cellHeight})
    }
    function scrollCueIntoView(cueIndex) {
        const map = collection.layout.cellOfCue
        if (cueIndex < 0 || cueIndex >= map.length) return
        positionViewAtIndex(map[cueIndex], GridView.Contain)
    }

    // Which SOURCE rows are actually visible, bottom (partial) row and the
    // one row of look-ahead included, fillers never. The host's ask and the
    // band report both go through this, so a gap between groups costs no
    // thumbnails.
    function visibleIndexes() {
        const cells = collection.layout.cells
        if (cellHeight <= 0 || cells.length === 0) return []
        const cols = Math.max(1, collection.columns)
        const first = Math.max(0, Math.floor(contentY / cellHeight) * cols)
        const rows = Math.ceil(height / cellHeight) + 1
        const out = []
        for (let i = first; i < Math.min(cells.length, first + rows * cols); ++i) {
            const cell = cells[i]
            if (cell.cueIndex !== undefined) out.push(cell.cueIndex)
        }
        return out
    }
    function visibleCueIndexes() { return collection.visibleIndexes() }

    // The ask is the host's business; the band is the view's. Every scroll
    // or resize that changes what is on screen restarts one debounced
    // report, so a wheel burst costs one ask at the destination.
    signal visibleBandChanged()
    Timer {
        id: bandReport
        interval: 80
        onTriggered: collection.visibleBandChanged()
    }
    onContentYChanged: bandReport.restart()
    onWidthChanged: bandReport.restart()
    onHeightChanged: bandReport.restart()

    clip: true
    keyNavigationEnabled: false  // the presenter's keys walk; the view must not eat them
    ScrollBar.vertical: ScrollBar {}
}
