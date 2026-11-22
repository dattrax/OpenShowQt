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
    objectName: "showPage"
    color: "#202020"
    signal editRequested()

    // Only Service combines the running order. An empty Library selection
    // stays empty; a selected item shows just its own slides.
    property string sourceFolder: "service"
    property string sourceShowId: ""
    property bool libraryPreview: false
    onSourceFolderChanged: cueIndex = -1
    onSourceShowIdChanged: cueIndex = -1
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
    // The running-order item that owns the rows below it is named on a band
    // in the gap above its first row. The band is the old one-line separator,
    // grown to carry the item's name once instead of repeating that name on
    // every slide's caption strip — which leaves the strips to the section
    // marking and the slide number alone.
    readonly property real groupHeaderHeight: 20
    readonly property real cueRowGap: 6
    // A cue card is the same chrome at every zoom step: the 16:9 picture plus
    // its caption strip. The row's cell adds the header band above it and the
    // gap below it, so the grid keeps one uniform row height.
    readonly property real cueCardHeight: (slideGrid.cellWidth - 12) * 9 / 16 + 36
    // Scheduling telemetry: how many times the band timer below has fired.
    // Scroll, zoom and document churn all push through that one timer, so a
    // burst costs one run rather than one per tick — the UI.3c suite pins it.
    property int bandRequests: 0
    // …and how many asks actually reached the worker: every run that was
    // standing down (page hidden, no thumbnail support) is counted by the
    // line above but never lands here. CS.9 pins the seam — a hidden Show
    // page must never ask, and the page catches up the moment it is shown.
    property int bandSent: 0

    // ND.9: the bin's camera number (1..4, creation order) per asset, so a
    // camera cue wears the same glyph the media card does.
    readonly property var cameraNumbers: {
        const numbers = ({})
        let number = 0
        for (const asset of docs.assets) {
            if (asset.kind !== "camera") continue
            numbers[asset.id] = ++number
        }
        return numbers
    }
    readonly property var cueRows: {
        showPage.cueRowsTick
        const groups = []
        if (showPage.libraryPreview) {
            groups.push({entryId: "", showId: "", showName: songService.previewTitle,
                         section: "", slides: songService.previewSlides})
        } else if (showPage.sourceShowId !== "") {
            const browsed = docs.showInfo(showPage.sourceShowId)
            if (browsed.id) {
                groups.push({entryId: "", showId: browsed.id, showName: browsed.name,
                             section: "", slides: docs.slidesOf(browsed.id)})
            }
        } else if (showPage.sourceFolder === "service") {
            for (const group of docs.runningOrderCues) groups.push(group)
        }
        const rows = []
        for (const group of groups) {
            let previousSection = null
            for (const slide of group.slides) {
                const sectionLabel = slide.sectionLabel || ""
                rows.push({cueKey: group.entryId + ":" + slide.id, entryId: group.entryId,
                           showId: group.showId, showName: group.showName,
                           section: group.section, slideId: slide.id,
                           slideIndex: slide.index, preview: slide.preview,
                           scene: slide.scene || "",
                           cameraNumber: showPage.cameraNumbers[slide.backgroundAssetId] || 0,
                           sectionLabel: sectionLabel,
                           sectionStart: previousSection !== sectionLabel.trim().toLowerCase(),
                           groupStart: slide.index === 0})
                previousSection = sectionLabel.trim().toLowerCase()
            }
        }
        return rows
    }
    readonly property string sourceName: {
        showPage.cueRowsTick
        if (showPage.libraryPreview) return songService.previewTitle
        if (showPage.sourceShowId !== "") {
            const browsed = docs.showInfo(showPage.sourceShowId)
            return browsed.name || ""
        }
        if (showPage.sourceFolder !== "service") return ""
        const list = docs.playlists.filter(p => p.id === docs.currentPlaylistId)[0]
        return list ? list.name : ""
    }

    // Task PT.7: whole-item theme application lives on the LEFT-HAND item
    // rows through ApplyThemeDialog. Cue tiles no longer carry a theme menu,
    // and the header no longer has a separate show-default picker: those were
    // alternate apply routes that skipped the whole-item rules. Per-slide
    // styling stays in Edit.

    // Triggering happens here and only here: compile the cue's slide (never
    // moving the editing cursor, UI.3a), then tag the go with the cue's key so
    // the worker's acknowledgement decides which tile lights.
    function presentCue(at) {
        if (at < 0 || at >= showPage.cueRows.length) return
        const row = showPage.cueRows[at]
        showPage.cueIndex = at
        const json = row.scene || docs.sceneJsonForSlide(row.slideId)
        if (json === "") return
        // Stage delivery FO.5b.1: the stage bundle is the CURRENT cue
        // compiled on the Stage role plus the immediate NEXT row's cue (the
        // same presentation sequence this grid walks). The controller files
        // it alongside the audience go and presents it only once that cue is
        // LIVE. Last row -> no next region; a direct/browsed go keeps only
        // its own cue.
        const next = at + 1 < showPage.cueRows.length ? showPage.cueRows[at + 1] : null
        const bundle = worker.stageCapable
            ? docs.stageBundleJson(row.showId, row.slideId,
                                   next ? next.showId : "", next ? next.slideId : "")
            : ""
        worker.presentCueWithStage(row.cueKey, json, bundle)
    }
    function stepCue(delta) {
        if (showPage.cueRows.length === 0) return
        const from = showPage.cueIndex >= 0 ? showPage.cueIndex : (delta > 0 ? -1 : 0)
        showPage.presentCue(Math.min(showPage.cueRows.length - 1, Math.max(0, from + delta)))
    }
    function jumpToCue(at) { showPage.presentCue(at) }

    // MB.3: a media card dropped on a cue sets that cue's slide background.
    // The cue names its own show/slide, so a tile belonging to another show
    // still edits the right slide. One edit, one undo; the editing cursor and
    // the presenter's place do not move.
    function applyMediaToCue(showId, slideId, assetId, fit) {
        if (showId === "" || slideId === "" || assetId === "") return false
        // Audio is not a cue's picture: a drop on a slide adds it to the
        // SHOW's audio layer, so every slide of that cue shares the track.
        const rows = docs.assets
        for (let i = 0; i < rows.length; ++i) {
            if (rows[i].id === assetId) {
                if (rows[i].kind === "audio")
                    return docs.setShowAudioFor(showId, assetId)
                break
            }
        }
        return docs.setSlideBackgroundFor(showId, slideId, assetId, fit || "cover")
    }

    // The service pane selects an occurrence, keeping the entire running
    // order visible. Wait for the source binding after leaving Library browse.
    function focusServiceEntry(entryId) {
        Qt.callLater(() => {
            const at = showPage.cueRows.findIndex(row => row.entryId === entryId)
            if (at < 0) return
            showPage.cueIndex = at
            slideGrid.scrollCueIntoView(at)
        })
    }

    // Rows within the ask envelope (UI.3c, mapped by CS.7): the collection
    // answers which SOURCE cues are actually on screen — bottom partial row
    // and one row of look-ahead included, group padding never — deduplicated
    // by slide id (a show listed twice shares one picture). The complete
    // window is retained for bounded multi-batch delivery; slideBand reports
    // its first 32-scene packet for the workspace checks. At the actual bottom
    // the last 32 unique pictures go first, with all earlier visible demand
    // still queued. Pure geometry: no worker or cache state enters this list.
    function slideWindow() {
        const rows = showPage.cueRows
        if (rows.length === 0) return []
        let wanted = slideGrid.visibleCueIndexes()
        if (wanted.length === 0) return []
        const atBottom = slideGrid.contentY + slideGrid.height >= slideGrid.contentHeight - 1
        if (atBottom) wanted = wanted.slice().reverse()
        const band = []
        const seen = ({})
        for (const i of wanted) {
            const id = rows[i].slideId
            if (seen[id]) continue
            seen[id] = true
            // TC.7: the compiled scene AND the render fingerprint its card is
            // cached under, from one compile. The consumer consults the
            // service's pictures first and only the misses reach the worker.
            const source = rows[i].scene ? {scene: rows[i].scene, fingerprint: ""}
                                        : docs.thumbnailSourceForSlide(id)
            band.push({key: id, scene: source.scene, fingerprint: source.fingerprint,
                       showId: rows[i].showId})
        }
        // Keep bottom priority without throwing away the earlier visible
        // cues. The consumer continues those after the first wire batch.
        if (atBottom) band.reverse()
        return band.length > 32 && atBottom
             ? band.slice(-32).concat(band.slice(0, -32)) : band
    }
    function slideBand() { return showPage.slideWindow().slice(0, 32) }
    // Operator cards use one 320x180 render at every zoom and panel width.
    // PreviewView scales it for display, so resizing never invalidates the
    // picture saved with the service. This fits the stream's slot envelope.
    function bandPixels() {
        return {w: 320, h: 180}
    }
    // Ask for the band. Every trigger — scroll, zoom, stepping, a document
    // edit, the page becoming visible — restarts the one timer below, so a
    // burst of wheel ticks costs one ask at the destination, not one per
    // tick, and the ask always describes where the grid ended up.
    function requestSlideBand() {
        ++showPage.bandRequests
        if (!visible) return
        // A warmed service shows its cards even when this worker cannot draw
        // thumbnails: persisted availability is its own capability (TC.7).
        if (!worker.thumbnailSupported && !worker.thumbnailCacheAvailable) return
        const rows = showPage.cueRows
        if (rows.length === 0) { worker.clearThumbnails(); return }
        const px = showPage.bandPixels()
        ++showPage.bandSent
        worker.requestThumbnailWindow(showPage.slideWindow(), px.w, px.h)
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
    // Scroll-to-cue follows the cue's MAPPED cell, not a raw model index
    // (CS.7): with group padding the two are different numbers.
    onCueIndexChanged: if (cueIndex >= 0) slideGrid.scrollCueIntoView(cueIndex)
    property bool bandAskedFor: false
    Connections {
        target: worker
        function onPreviewChanged() {
            if ((worker.thumbnailSupported || worker.thumbnailCacheAvailable)
                    && !showPage.bandAskedFor) {
                showPage.bandAskedFor = true
                thumbnailAsk.restart()
            }
        }
    }
    onVisibleChanged: {
        if (visible) thumbnailAsk.restart()
        else {
            thumbnailAsk.stop()
            worker.clearThumbnails()
        }
    }
    Component.onCompleted: thumbnailAsk.restart()

    Rectangle {
        id: heading
        anchors.top: parent.top
        width: parent.width; height: 38
        color: "#141414"
        Text {
            anchors.left: parent.left; anchors.leftMargin: 14
            anchors.right: thumbSize.left; anchors.rightMargin: 12
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

    SlidePreviewCollection {
        id: slideGrid
        objectName: "operatorSlideGrid"
        anchors.top: heading.bottom
        anchors.bottom: transport.top
        anchors.left: parent.left; anchors.right: parent.right
        anchors.margins: 12
        // CS.7: the grid groups by running-order ENTRY occurrence. Two
        // copies of one show are two groups, each starting at column zero,
        // so the grid shows the order the service will actually walk.
        grouped: true
        stretchCells: true
        cardWidth: showPage.thumbCellWidth
        // One uniform row: the header band, the card and the gap under it.
        cellHeight: showPage.cueCardHeight + showPage.groupHeaderHeight + showPage.cueRowGap
        source: showPage.cueRows
        onVisibleBandChanged: thumbnailAsk.restart()
        // A cue tile is a click target, not a focusable button: an
        // AbstractButton would squat the presenter's Space after every
        // click, and clicking a cue must leave the keyboard still walking.
        // Task CS.2 put that tile in the shared card: the state border and
        // caption strip are the card's chrome, the picture is the shared
        // surface on the keyed Thumbnail target, and what a click MEANS
        // (present this cue) stays here, on the Show screen.
        delegate: Item {
            id: cell
            required property var modelData
            // Attached delegate properties resolve inside THIS body only,
            // so the row and the cue index are re-exported for the card.
            // A filler cell hands the card an empty record: bindings stay
            // quiet and the whole cell stays invisible and inert.
            readonly property var emptyRow: ({cueKey: "", showName: "", section: "",
                                              slideId: "", slideIndex: -1, preview: "",
                                              cameraNumber: 0,
                                              sectionLabel: "", sectionStart: false,
                                              groupStart: false})
            readonly property var row: modelData.filler === true
                                       ? cell.emptyRow : modelData.row
            readonly property int cue: modelData.filler === true
                                       ? -1 : modelData.cueIndex
            width: slideGrid.cellWidth
            height: slideGrid.cellHeight
            // Group padding is INERT: no name, no picture, no clicks, no
            // context menu, and nothing for a test to find.
            visible: modelData.filler !== true
            // The boundary between service items: the one-line rule has grown
            // into a full-width band in the gap above the group's first row,
            // naming the item on a single line. Every slide strip below it is
            // then free of the repeated name.
            Rectangle {
                id: groupHeader
                objectName: "serviceGroupHeader"
                readonly property string groupName:
                    (cell.row.section !== "" ? cell.row.section + " \u00b7 " : "")
                    + cell.row.showName
                visible: cell.cue >= 0 && cell.row.groupStart
                         && showPage.sourceFolder === "service"
                         && showPage.sourceShowId === ""
                x: -cell.x
                y: 0
                width: slideGrid.width
                height: showPage.groupHeaderHeight
                color: "#1e1e1e"
                Rectangle {
                    width: parent.width
                    height: 1
                    color: "#737373"
                }
                Text {
                    objectName: "serviceGroupHeaderText"
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    verticalAlignment: Text.AlignVCenter
                    text: groupHeader.groupName
                    color: "#e6e6e6"
                    font.pixelSize: 11
                    font.bold: true
                    elide: Text.ElideRight
                }
            }
            SlidePreviewCard {
                id: tile
                objectName: "operatorCueTile"
                readonly property var modelData: cell.row
                readonly property int index: cell.cue
                x: 0
                y: showPage.groupHeaderHeight
                width: slideGrid.cellWidth - 12
                height: showPage.cueCardHeight
                readonly property string rowCueKey: modelData.cueKey
                readonly property bool rowSelected: index === showPage.cueIndex
                readonly property bool rowLive: modelData.cueKey !== ""
                                                && modelData.cueKey === worker.liveCueKey
                readonly property bool rowPending: modelData.cueKey !== ""
                                                   && modelData.cueKey === worker.pendingCueKey
                selected: rowSelected
                live: rowLive
                sectionLabel: modelData.sectionLabel
                showSectionLabel: modelData.sectionStart
                ordinal: String(modelData.slideIndex + 1)
                caption: modelData.preview
                Accessible.role: Accessible.Button
                Accessible.name: "Cue " + (index + 1) + ": " + modelData.showName
                                 + (modelData.section !== "" ? ", " + modelData.section : "")
                                 + (tile.rowLive ? ", live"
                                    : (tile.rowPending ? ", presenting" : ""))
                surface.controller: worker
                surface.target: "thumbnail"
                surface.key: modelData.slideId
                surface.pixelsVisible: worker.thumbnailSupported
                                       || worker.thumbnailCacheAvailable
                // With neither a worker lane nor persisted pictures the
                // operator still sees the words the cue carries — a text row,
                // plainly styled, never advertised as the rendered slide.
                surface.placeholder: modelData.preview
                surface.placeholderVisible: !worker.thumbnailSupported
                                            && !worker.thumbnailCacheAvailable
                surface.placeholderColor: "#eeeeee"
                surface.placeholderPixelSize: 13

                // ND.9: an off-air camera has no frame to show (its receiver
                // opens only for the scene the audience is on), so the cue
                // wears the Media Bin's own camera glyph rather than a black
                // tile. The live cue shows its feed instead.
                CameraArtwork {
                    objectName: "cueCameraArtwork"
                    visible: tile.modelData.cameraNumber > 0 && !tile.rowLive
                    anchors.centerIn: parent
                    width: Math.min(parent.width, parent.height) * 0.55
                    height: width
                    number: tile.modelData.cameraNumber
                }

                // The acknowledgement chip: only ever shown from the worker's
                // own state, never from the click that asked.
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

                onActivated: showPage.presentCue(index)
            }

            // MB.3: a media card dropped on this cue sets its slide background.
            DropArea {
                id: mediaDrop
                objectName: "cueMediaDrop"
                anchors.fill: parent
                enabled: cell.visible
                keys: ["text/plain"]
                readonly property string dropHint: "Set this cue's background"
                // The bin's proxy carries the payload on the SOURCE (the
                // DragFeedback's own `payload`), exactly like every other
                // internal drop target in this workspace. Qt does not expose
                // it through DragEvent.text for an internal Quick drag — that
                // reads empty beside a populated source — so the source is
                // read first and the mime text only answers a foreign drag.
                function payloadOf(event) {
                    const carried = event.source ? String(event.source.payload || "") : ""
                    return carried !== "" ? carried
                                          : String(event.getDataAsString("text/plain") || "")
                }
                onEntered: function (drag) {
                    const payload = mediaDrop.payloadOf(drag)
                    if (payload.startsWith("media:")) {
                        drag.accept(Qt.CopyAction)
                        drag.accepted = true
                    } else {
                        drag.accepted = false
                    }
                }
                onDropped: function (drop) {
                    const payload = mediaDrop.payloadOf(drop)
                    if (!payload.startsWith("media:")) return
                    showPage.applyMediaToCue(cell.row.showId, cell.row.slideId,
                                            payload.substring("media:".length),
                                            "cover")
                }
            }
            Rectangle {
                anchors.fill: parent
                visible: mediaDrop.containsDrag
                color: "#331685f8"
                border.color: "#65c4ff"
                border.width: 2
            }
        }
    }
    Column {
        anchors.centerIn: slideGrid
        width: Math.max(0, slideGrid.width - 40)
        spacing: 10
        visible: showPage.cueRows.length === 0
        Text {
            objectName: "operatorEmptyTitle"
            width: parent.width; horizontalAlignment: Text.AlignHCenter
            color: "#dddddd"; font.pixelSize: 18
            text: showPage.libraryPreview ? songService.previewStatus
                  : showPage.sourceShowId !== "" ? "This item has no slides"
                  : showPage.sourceFolder === "songs" ? "Select a song"
                  : showPage.sourceFolder === "presentations" ? "Select a presentation"
                  : "This service is empty"
        }
        Text {
            width: parent.width; horizontalAlignment: Text.AlignHCenter
            color: "#aaaaaa"; font.pixelSize: 12; wrapMode: Text.WordWrap
            text: showPage.sourceFolder === "service" && showPage.sourceShowId === ""
                  ? "Add songs or presentations to the service on the left."
                  : "Choose an item in the Library to preview its slides."
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
            enabled: !showPage.libraryPreview && docs.currentSongId !== ""
            onClicked: showPage.editRequested()
        }
    }
}
