import QtQuick
import QtQuick.Controls

// Presenting only. Slide text, authoring commands and history live in EditPage.
// UI.3b: the grid shows the CHOSEN running order's cues — or one browsed
// presentation — independent of where the editing cursor sits. Clicking a cue
// presents it; stepping walks the running order across presentation
// boundaries and repeated entries. Selection, the acknowledged LIVE marker and
// the owed PENDING marker are three separate facts: there is no follow-arming
// state, and nothing lights live until the worker says the cue's revision is
// on air.
Rectangle {
    id: showPage
    color: "#202020"
    signal editRequested()

    // "" follows the running order under preparation; any other value is a
    // presentation browsed from the library — browsing, never presenting.
    property string sourceShowId: ""
    // The cue the operator last chose. A selection, not the output: the live
    // cue is worker.liveCueKey's answer, and the two are allowed to differ.
    property int cueIndex: -1

    // Bumps on any document edit so the derived row list rebuilds even though
    // slidesOf() is a call rather than a tracked property.
    property int cueRowsTick: 0

    // UI.3c: the operator sizes the thumbnails; the choice is remembered in
    // AppSettings like the panel widths. Five steps — step 2 is the 190-pixel
    // cell the grid has always had, so the default view does not move until
    // the operator asks for one.
    readonly property var thumbSteps: [120, 150, 190, 240, 300]
    readonly property int thumbLevel: appSettings.showThumbLevel
    readonly property int thumbCellWidth:
        thumbSteps[Math.max(0, Math.min(thumbSteps.length - 1, thumbLevel))]
    // Scheduling telemetry: how many times the band timer below has fired.
    // Scroll, zoom and document churn all push through that one timer, so a
    // burst costs one run rather than one per tick — the UI.3c suite pins it.
    property int bandRequests: 0

    readonly property var cueRows: {
        showPage.cueRowsTick
        const groups = []
        if (showPage.sourceShowId !== "") {
            const browsed = docs.songs.filter(s => s.id === showPage.sourceShowId)[0]
            if (browsed) {
                groups.push({entryId: "", showId: browsed.id, showName: browsed.name,
                             section: "", slides: docs.slidesOf(browsed.id)})
            }
        } else {
            for (const group of docs.runningOrderCues) groups.push(group)
            if (groups.length === 0 && docs.currentSongId !== "") {
                // A fresh service whose order has nothing in it yet still
                // shows the selected presentation rather than an empty wall.
                const current = docs.songs.filter(s => s.id === docs.currentSongId)[0]
                if (current) {
                    groups.push({entryId: "", showId: current.id, showName: current.name,
                                 section: "", slides: docs.slidesOf(current.id)})
                }
            }
        }
        const rows = []
        for (const group of groups)
            for (const slide of group.slides)
                rows.push({cueKey: group.entryId + ":" + slide.id, entryId: group.entryId,
                           showId: group.showId, showName: group.showName,
                           section: group.section, slideId: slide.id,
                           slideIndex: slide.index, preview: slide.preview,
                           groupStart: slide.index === 0})
        return rows
    }
    readonly property string sourceName: {
        if (showPage.sourceShowId !== "") {
            const browsed = docs.songs.filter(s => s.id === showPage.sourceShowId)[0]
            return browsed ? browsed.name : ""
        }
        const list = docs.playlists.filter(p => p.id === docs.currentPlaylistId)[0]
        return list ? list.name : ""
    }

    // Triggering happens here and only here: compile the cue's slide (never
    // moving the editing cursor, UI.3a), then tag the go with the cue's key so
    // the worker's acknowledgement decides which tile lights.
    function presentCue(at) {
        if (at < 0 || at >= showPage.cueRows.length) return
        const row = showPage.cueRows[at]
        showPage.cueIndex = at
        const json = docs.sceneJsonForSlide(row.slideId)
        if (json !== "") worker.presentCueJson(row.cueKey, json)
    }
    function stepCue(delta) {
        if (showPage.cueRows.length === 0) return
        const from = showPage.cueIndex >= 0 ? showPage.cueIndex : (delta > 0 ? -1 : 0)
        showPage.presentCue(Math.min(showPage.cueRows.length - 1, Math.max(0, from + delta)))
    }
    function jumpToCue(at) { showPage.presentCue(at) }

    // Rows within the ask envelope (UI.3c): from the first visible row to one
    // row past the viewport, deduplicated by slide id — a show listed twice
    // shares one picture — and capped at the worker's 32-scene batch. When
    // the cap bites, the window anchors off the BOTTOM edge instead, so a
    // scrolled-to-end grid never finds its visible rows outside the band.
    // Pure function of the current geometry, so the offscreen suite can pin
    // the extent, the cap and the dedupe without a worker on the wire.
    function slideBand() {
        const rows = showPage.cueRows
        if (rows.length === 0) return []
        const first = Math.max(0, Math.floor(slideGrid.contentY / slideGrid.cellHeight))
                      * slideGrid.columns
        const count = Math.min(32, (Math.ceil(slideGrid.height / slideGrid.cellHeight) + 1)
                                  * slideGrid.columns)
        const lastVisible = Math.min(rows.length,
                                     (first / slideGrid.columns
                                      + Math.ceil(slideGrid.height / slideGrid.cellHeight))
                                     * slideGrid.columns)
        const start = lastVisible >= rows.length
                      ? Math.max(0, rows.length - count) : first
        const band = []
        const seen = ({})
        for (let i = start; i < Math.min(rows.length, start + count); ++i) {
            const id = rows[i].slideId
            if (seen[id]) continue
            seen[id] = true
            band.push({key: id, scene: docs.sceneJsonForSlide(id)})
        }
        return band
    }
    // The ask tracks the cell (UI.3c): 16:9 at the tile's picture width, held
    // inside the envelope every 640x360 preview slot accepts — the protocol
    // caps each dimension at 32..512, and 512x288 is the largest 16:9 frame
    // whose pixels fit the slot this app starts the stream at, so a size this
    // control picks can never be refused on the wire.
    function bandPixels() {
        const w = Math.max(160, Math.min(512, Math.round(slideGrid.cellWidth - 18)))
        return {w: w, h: Math.round(w * 9 / 16)}
    }
    // Ask for the band. Every trigger — scroll, zoom, stepping, a document
    // edit, the page becoming visible — restarts the one timer below, so a
    // burst of wheel ticks costs one ask at the destination, not one per
    // tick, and the ask always describes where the grid ended up.
    function requestSlideBand() {
        ++showPage.bandRequests
        if (!visible || !worker.thumbnailSupported) return
        const rows = showPage.cueRows
        if (rows.length === 0) { worker.clearThumbnails(); return }
        const px = showPage.bandPixels()
        worker.requestThumbnails(showPage.slideBand(), px.w, px.h)
    }
    Timer { id: thumbnailAsk; interval: 80; onTriggered: showPage.requestSlideBand() }
    onThumbCellWidthChanged: thumbnailAsk.restart()
    Connections {
        target: docs
        function onDocumentsChanged() { showPage.cueRowsTick++ }
        function onSlideTextChanged() { showPage.cueRowsTick++ }
    }
    onCueRowsChanged: {
        if (showPage.cueIndex >= showPage.cueRows.length) showPage.cueIndex = -1
        thumbnailAsk.restart()
    }
    onCueIndexChanged: slideGrid.positionViewAtIndex(cueIndex, GridView.Contain)
    property bool bandAskedFor: false
    Connections {
        target: worker
        function onPreviewChanged() {
            if (worker.thumbnailSupported && !showPage.bandAskedFor) {
                showPage.bandAskedFor = true
                thumbnailAsk.restart()
            }
        }
    }
    onVisibleChanged: if (visible) thumbnailAsk.restart()
    Component.onCompleted: thumbnailAsk.restart()

    Rectangle {
        id: heading
        anchors.top: parent.top
        width: parent.width; height: 38
        color: "#141414"
        Text {
            anchors.left: parent.left; anchors.leftMargin: 14
            anchors.right: slideCount.left; anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            text: showPage.sourceName || "Nothing selected"
            color: "#eeeeee"; font.pixelSize: 14; font.bold: true
            elide: Text.ElideRight
        }
        Text {
            id: slideCount
            anchors.right: parent.right; anchors.rightMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            color: "#aaaaaa"; font.pixelSize: 11
            text: showPage.cueRows.length + (showPage.cueRows.length === 1 ? " cue" : " cues")
        }
        // The only view control the page has, because it is the only one it
        // can honour (UI.3c): the two buttons resize the cells, and the
        // re-ask follows automatically through the band timer. PanelButton is
        // Qt.NoFocus, so the presenter keeps its keys after every click.
        Row {
            id: thumbSize
            objectName: "operatorThumbSize"
            anchors.right: slideCount.left; anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            spacing: 4
            PanelButton {
                objectName: "operatorThumbSmaller"
                text: "\u2212"
                enabled: showPage.thumbLevel > 0
                Accessible.name: "Smaller thumbnails"
                onClicked: appSettings.showThumbLevel = showPage.thumbLevel - 1
            }
            PanelButton {
                objectName: "operatorThumbLarger"
                text: "+"
                enabled: showPage.thumbLevel < showPage.thumbSteps.length - 1
                Accessible.name: "Larger thumbnails"
                onClicked: appSettings.showThumbLevel = showPage.thumbLevel + 1
            }
        }
    }

    GridView {
        id: slideGrid
        objectName: "operatorSlideGrid"
        anchors.top: heading.bottom
        anchors.bottom: transport.top
        anchors.left: parent.left; anchors.right: parent.right
        anchors.margins: 12
        clip: true
        // The cue area never holds focus: a focused view eats PageUp/PageDown
        // as navigation, and Space/arrows must keep walking cues after any
        // click inside the grid.
        keyNavigationEnabled: false
        readonly property int columns: Math.max(1, Math.floor(width / showPage.thumbCellWidth))
        cellWidth: width / columns
        cellHeight: (cellWidth - 12) * 9 / 16 + 48
        model: showPage.cueRows
        onContentYChanged: thumbnailAsk.restart()
        onWidthChanged: thumbnailAsk.restart()
        onHeightChanged: thumbnailAsk.restart()
        ScrollBar.vertical: ScrollBar {}
        // A cue tile is a click target, not a focusable button: an
        // AbstractButton would squat the presenter's Space after every
        // click, and clicking a cue must leave the keyboard still walking.
        delegate: Item {
            id: tile
            objectName: "operatorCueTile"
            required property var modelData
            required property int index
            width: slideGrid.cellWidth - 12
            height: slideGrid.cellHeight - 12
            readonly property string rowCueKey: modelData.cueKey
            readonly property bool rowSelected: index === showPage.cueIndex
            readonly property bool rowLive: modelData.cueKey !== ""
                                            && modelData.cueKey === worker.liveCueKey
            readonly property bool rowPending: modelData.cueKey !== ""
                                               && modelData.cueKey === worker.pendingCueKey
            Accessible.role: Accessible.Button
            Accessible.name: "Cue " + (index + 1) + ": " + modelData.showName
                             + (modelData.section !== "" ? ", " + modelData.section : "")
                             + (tile.rowLive ? ", live"
                                : (tile.rowPending ? ", presenting" : ""))
            Rectangle {
                anchors.fill: parent
                color: tileMouse.containsMouse ? "#393939" : "#2d2d2d"
                // Live is the worker's fact (red, thickest); the operator's
                // own pick is blue; a pending go wears its chip, not a border.
                border.width: tile.rowLive ? 3 : (tile.rowSelected ? 2 : 1)
                border.color: tile.rowLive ? "#d24b4b"
                             : (tile.rowSelected ? "#1685f8" : "#505050")
            }
            Item {
                anchors.fill: parent
                Rectangle {
                    id: picture
                    anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
                    anchors.margins: 3
                    height: width * 9 / 16
                    color: "black"
                    Text {
                        anchors.fill: parent; anchors.margins: 12
                        visible: !worker.thumbnailSupported
                        text: tile.modelData.preview
                        color: "#eeeeee"; font.pixelSize: 13
                        wrapMode: Text.WordWrap
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    PreviewView {
                        visible: worker.thumbnailSupported
                        anchors.fill: parent
                        controller: worker
                        target: PreviewView.Thumbnail
                        key: tile.modelData.slideId
                    }
                    // The acknowledgement chip: only ever shown from the
                    // worker's own state, never from the click that asked.
                    Rectangle {
                        visible: tile.rowLive || tile.rowPending
                        anchors.top: parent.top; anchors.left: parent.left
                        anchors.margins: 5
                        width: chipLabel.implicitWidth + 10; height: 18
                        radius: 3
                        color: tile.rowLive ? "#d24b4b" : "#8a6d1f"
                        Text {
                            id: chipLabel
                            anchors.centerIn: parent
                            font.pixelSize: 10; font.bold: true
                            color: "#ffffff"
                            text: tile.rowLive ? "LIVE" : "GO\u2026"
                        }
                    }
                }
                Rectangle {
                    anchors.top: picture.bottom; anchors.bottom: parent.bottom
                    anchors.left: parent.left; anchors.right: parent.right
                    anchors.margins: 2
                    color: tile.rowLive ? "#7c2c28"
                         : (tile.rowSelected ? "#1678cf" : "#414141")
                    Text {
                        anchors.fill: parent; anchors.leftMargin: 6; anchors.rightMargin: 6
                        verticalAlignment: Text.AlignVCenter
                        text: (tile.modelData.groupStart && showPage.sourceShowId === ""
                               ? tile.modelData.showName + " \u00b7 " : "")
                              + (tile.modelData.slideIndex + 1) + "  " + tile.modelData.preview
                        color: "#eeeeee"; font.pixelSize: 12
                        elide: Text.ElideRight
                    }
                }
            }
            MouseArea {
                id: tileMouse
                anchors.fill: parent
                hoverEnabled: true
                acceptedButtons: Qt.LeftButton
                onClicked: showPage.presentCue(index)
            }
        }
    }
    Column {
        anchors.centerIn: slideGrid
        width: Math.max(0, slideGrid.width - 40)
        spacing: 10
        visible: showPage.cueRows.length === 0
        Text {
            width: parent.width; horizontalAlignment: Text.AlignHCenter
            color: "#dddddd"; font.pixelSize: 18
            text: showPage.sourceShowId !== "" ? "This presentation has no slides"
                  : docs.currentPlaylistId === "" && docs.currentSongId === ""
                    ? "Choose a presentation" : "This running order is empty"
        }
        Text {
            width: parent.width; horizontalAlignment: Text.AlignHCenter
            color: "#aaaaaa"; font.pixelSize: 12; wrapMode: Text.WordWrap
            text: showPage.sourceShowId !== "" || docs.currentSongId !== ""
                  ? "Add shows to the running order on the left, or open Edit to write slides."
                  : "Select an item in the library or running order."
        }
    }
    Rectangle {
        id: transport
        anchors.bottom: parent.bottom
        width: parent.width; height: 52
        color: "#2c2c2c"
        Row {
            anchors.left: parent.left; anchors.leftMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            spacing: 6
            PanelButton {
                text: "\u25c0"
                Accessible.name: "Previous cue"
                enabled: showPage.cueRows.length > 0
                onClicked: showPage.stepCue(-1)
            }
            PanelButton {
                text: "\u25b6"
                Accessible.name: "Next cue"
                enabled: showPage.cueRows.length > 0
                onClicked: showPage.stepCue(1)
            }
            PanelButton {
                text: "Present"
                highlighted: true
                enabled: showPage.cueRows.length > 0 && worker.outputLive
                onClicked: showPage.presentCue(showPage.cueIndex >= 0 ? showPage.cueIndex : 0)
            }
        }
        PanelButton {
            anchors.right: parent.right; anchors.rightMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            text: "Edit"
            visible: showPage.width >= 530
            enabled: docs.currentSongId !== ""
            onClicked: showPage.editRequested()
        }
    }
}
