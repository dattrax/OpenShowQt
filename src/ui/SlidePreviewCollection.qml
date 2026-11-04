import QtQuick
import QtQuick.Controls

/*
 * UI part: SlidePreviewCollection; contract: docs/ui/control-sets.md#slidepreviewcollection
 *
 * A virtualized responsive collection of SlidePreviewCards with index
 * mapping and visible-band reporting (task CS.3 introduces the ungrouped
 * mode for the Bible import; CS.7 gives the Show screen its grouped rows,
 * where every running-order item occurrence starts a new visual row).
 *
 * The collection displays and maps; it never schedules scene requests and
 * never reorders cues. Hosts keep their own flat presentation-order model
 * and read the band this view actually shows, so the ask describes where
 * the operator is looking rather than where a counter happens to be.
 */
GridView {
    id: collection

    // Grouped mode (CS.7): every item occurrence starts a new row. The
    // ungrouped flow — one uniform grid — is what the Bible import lists
    // today, and it is the honest default until the grouped row-break
    // arithmetic and its viewport mapping land with the Show migration.
    property bool grouped: false
    // Operator zoom: the width one card asks for, in window pixels. The
    // GridView's own cellWidth follows it; the view stays the only place
    // the cell arithmetic is spelled.
    property real cardWidth: 220

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

    // Which model rows are actually visible, bottom row included. In the
    // ungrouped flow a visual cell IS a model row — that identity is the
    // point; the CS.7 grouped mode replaces it with the mapped geometry,
    // and hosts go through THIS function either way so their ask never
    // has to know which era of layout they are in.
    function visibleIndexes() {
        if (cellHeight <= 0 || model.length === 0) return []
        const cols = Math.max(1, Math.floor(width / cardWidth))
        const first = Math.max(0, Math.floor(contentY / cellHeight) * cols)
        const rows = Math.ceil(height / cellHeight) + 1
        const out = []
        for (let i = first; i < Math.min(model.length, first + rows * cols); ++i)
            out.push(i)
        return out
    }

    readonly property int columns: Math.max(1, Math.floor(width / cardWidth))
    cellWidth: cardWidth
    cellHeight: (cardWidth - 12) * 9 / 16 + 48
    clip: true
    keyNavigationEnabled: false  // the presenter's keys walk; the view must not eat them
    ScrollBar.vertical: ScrollBar {}
}
