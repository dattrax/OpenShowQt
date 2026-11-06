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
    // …and how many asks actually reached the worker: every run that was
    // standing down (page hidden, no thumbnail support) is counted by the
    // line above but never lands here. CS.9 pins the seam — a hidden Show
    // page must never ask, and the page catches up the moment it is shown.
    property int bandSent: 0

    readonly property var cueRows: {
        showPage.cueRowsTick
        const groups = []
        if (showPage.sourceShowId !== "") {
            const browsed = docs.showInfo(showPage.sourceShowId)
            if (browsed.id) {
                groups.push({entryId: "", showId: browsed.id, showName: browsed.name,
                             section: "", slides: docs.slidesOf(browsed.id)})
            }
        } else {
            for (const group of docs.runningOrderCues) groups.push(group)
            if (groups.length === 0 && docs.currentSongId !== "") {
                // A fresh service whose order has nothing in it yet still
                // shows the selected presentation rather than an empty wall.
                const current = docs.showInfo(docs.currentSongId)
                if (current.id) {
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
        showPage.cueRowsTick
        if (showPage.sourceShowId !== "") {
            const browsed = docs.showInfo(showPage.sourceShowId)
            return browsed.name || ""
        }
        const list = docs.playlists.filter(p => p.id === docs.currentPlaylistId)[0]
        return list ? list.name : ""
    }

    // The show whose cues are on screen (task TH.5): the browsed
    // presentation, or the one under preparation when the grid follows the
    // running order. The header's theme selector reads and sets THIS
    // show's default — browsing Beta must never restyle Alpha.
    readonly property string viewedShowId:
        showPage.sourceShowId !== "" ? showPage.sourceShowId : docs.currentSongId
    // The default as the model spells it, and as the header button names it.
    function showDefaultId() {
        showPage.cueRowsTick
        return docs.showInfo(showPage.viewedShowId).defaultThemeId || ""
    }
    function showDefaultName() {
        const theme = docs.themes.filter(t => t.id === showPage.showDefaultId())[0]
        return theme ? theme.name : "\u2014 no show default \u2014"
    }
    // The tile a right-click opened the context menu on: the menu acts on
    // the slide it was asked for, not on whatever the cursor says later.
    property string contextSlideId: ""

    // The cue tile's context menu (task TH.5): theme the one slide, or
    // put its customizations back. Flat rows under a separator — nested
    // submenus cannot hover themselves open for a keyboard, a test, or a
    // slow operator. Both actions are ordinary document edits on the
    // slide's own show, refused on the status line when the names have
    // died between the click and the trigger.
    Menu {
        id: cueThemeMenu
        objectName: "cueThemeMenu"
        MenuItem {
            objectName: "cueFollowShowItem"
            text: "\u2014 follow show default \u2014"
            onTriggered: docs.applyThemeToSlide(showPage.contextSlideId, "")
        }
        Repeater {
            model: docs.themes
            MenuItem {
                required property var modelData
                objectName: "cueApplyThemeItem"
                text: modelData.name
                onTriggered: docs.applyThemeToSlide(showPage.contextSlideId,
                                                    modelData.id)
            }
        }
        MenuSeparator { objectName: "cueMenuSeparator" }
        MenuItem {
            objectName: "cueRemoveCustomizationItem"
            text: "Remove customizations"
            onTriggered: docs.removeSlideCustomizationsFor(showPage.contextSlideId)
        }
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
            band.push({key: id, scene: docs.sceneJsonForSlide(id)})
        }
        // Keep bottom priority without throwing away the earlier visible
        // cues. The consumer continues those after the first wire batch.
        if (atBottom) band.reverse()
        return band.length > 32 && atBottom
             ? band.slice(-32).concat(band.slice(0, -32)) : band
    }
    function slideBand() { return showPage.slideWindow().slice(0, 32) }
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
            if (worker.thumbnailSupported && !showPage.bandAskedFor) {
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
            anchors.right: showThemeSelector.left; anchors.rightMargin: 12
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
        // The show default selector (task TH.5): which theme the cues
        // that follow the show draw when they wear nothing closer. It is
        // the VIEWED show's default — while browsing the library this is
        // that presentation's own setting, not the editing cursor's.
        // A button over a menu, like the header's More tab: the Show
        // screen keeps no text entry, and the menu lists what the model
        // has, the worn default checked.
        // A plain AbstractButton with an explicit popup, exactly like the
        // header's More tab: the Show screen keeps no text entry, and the
        // menu lists what the model has, the worn default checked.
        AbstractButton {
            id: showThemeSelector
            objectName: "showThemeSelector"
            anchors.right: thumbSize.left; anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            width: Math.min(220, label.implicitWidth + 20)
            height: 28
            padding: 0
            focusPolicy: Qt.NoFocus
            enabled: showPage.viewedShowId !== ""
            text: showPage.showDefaultName()
            Accessible.name: "Show default theme"
            onClicked: showDefaultThemeMenu.popup()
            background: Rectangle {
                radius: 3
                color: showThemeSelector.hovered ? "#575757" : "#454545"
                border.color: "#606060"
            }
            contentItem: Text {
                id: label
                text: showThemeSelector.text
                color: showThemeSelector.enabled ? "#eeeeee" : "#888888"
                font.pixelSize: 12
                verticalAlignment: Text.AlignVCenter
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
            }
            Menu {
                id: showDefaultThemeMenu
                objectName: "showDefaultThemeMenu"
                MenuItem {
                    objectName: "showDefaultNoneItem"
                    text: (showPage.showDefaultId() === "" ? "\u2713 " : "")
                          + "\u2014 no show default \u2014"
                    onTriggered: docs.setShowDefaultThemeFor(showPage.viewedShowId, "")
                }
                Repeater {
                    model: docs.themes
                    MenuItem {
                        required property var modelData
                        objectName: "showDefaultThemeItem"
                        text: (showPage.showDefaultId() === modelData.id ? "\u2713 " : "")
                              + modelData.name
                        onTriggered: docs.setShowDefaultThemeFor(showPage.viewedShowId,
                                                                 modelData.id)
                    }
                }
            }
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
            SlidePreviewCard {
                id: tile
                objectName: "operatorCueTile"
                readonly property var modelData: cell.row
                readonly property int index: cell.cue
                width: slideGrid.cellWidth - 12
                height: slideGrid.cellHeight - 12
                readonly property string rowCueKey: modelData.cueKey
                readonly property bool rowSelected: index === showPage.cueIndex
                readonly property bool rowLive: modelData.cueKey !== ""
                                                && modelData.cueKey === worker.liveCueKey
                readonly property bool rowPending: modelData.cueKey !== ""
                                                   && modelData.cueKey === worker.pendingCueKey
                selected: rowSelected
                live: rowLive
                caption: (modelData.groupStart && showPage.sourceShowId === ""
                          ? modelData.showName + " \u00b7 " : "")
                         + (modelData.slideIndex + 1) + "  " + modelData.preview
                Accessible.role: Accessible.Button
                Accessible.name: "Cue " + (index + 1) + ": " + modelData.showName
                                 + (modelData.section !== "" ? ", " + modelData.section : "")
                                 + (tile.rowLive ? ", live"
                                    : (tile.rowPending ? ", presenting" : ""))
                surface.controller: worker
                surface.target: "thumbnail"
                surface.key: modelData.slideId
                surface.pixelsVisible: worker.thumbnailSupported
                // With no thumbnail lane the operator still sees the words the
                // cue carries — a text row, plainly styled, never advertised as
                // the rendered slide.
                surface.placeholder: modelData.preview
                surface.placeholderVisible: !worker.thumbnailSupported
                surface.placeholderColor: "#eeeeee"
                surface.placeholderPixelSize: 13

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
                onContextRequested: function (mouseX, mouseY) {
                    // The menu acts on the tile that was clicked, at the moment
                    // it was clicked — capture then.
                    showPage.contextSlideId = tile.modelData.slideId
                    const at = tile.mapToItem(showPage, mouseX, mouseY)
                    cueThemeMenu.popup(at.x, at.y)
                }
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
