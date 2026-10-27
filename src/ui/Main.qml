import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material

/*
 * The operator workspace (M4 task 4.1): the reference's shell — a 40px
 * top bar over a row of left panel / center / right panel — with the
 * lyric flow built through M3 living inside it. The side panels are the
 * reference's resizable ones (SidePanel.qml + WorkspacePanels' measured
 * snap rules); the centre carries the slide list and the editor, and
 * the right panel is always the preview with the output tools below it,
 * like the reference's Preview + ShowTools.
 *
 * Theme tokens are the reference palette (docs/reference.md §3).
 */
Window {
    id: root
    width: uiSize.width
    height: uiSize.height
    minimumWidth: 400   // the reference's window minimum
    minimumHeight: 400
    visible: true
    color: "#191923"  // --primary-darker, the canvas behind everything
    title: "OpenShowQt " + applicationVersion
    // Task 4.5: the F11/settings fullscreen is a persisted app setting,
    // so a projector setup survives restarts.
    visibility: appSettings.fullscreen ? Window.FullScreen : Window.Windowed

    Material.theme: Material.Dark
    Material.accent: "#f0008c"  // --secondary

    // Task 1.2: closing the UI shuts the worker down before the window
    // actually goes away.
    onClosing: function(close) {
        worker.shutdownNow()
    }

    // The workspace page under the top bar (4.3): "show" is the running
    // order and lyric editor, "edit" the text-box canvas. The remaining
    // reference pages arrive later and their buttons stay disabled.
    property string page: "show"

    // True while a text field holds the focus — the plain-arrow nudges
    // must not steal keystrokes from someone typing.
    readonly property bool typingIntoField: activeFocusItem !== null
        && (activeFocusItem instanceof TextField || activeFocusItem instanceof TextArea
            || activeFocusItem instanceof TextInput)

    // Task 3.3: once a slide has been presented from the panel, navigating
    // follows it onto the output — that is the operator's expectation
    // mid-service. Any Present click (re)arms it.
    property bool following: false

    // Panel building blocks (task 4.4): Material's default TextField is
    // too tall for a 250px column once the background is replaced (the
    // placeholder and text split vertically), and its Button is too wide
    // for three in a row. These carry the workspace palette and metrics.
    component PanelField: TextField {
        color: "#f0f0ff"
        font.pixelSize: 13
        height: 30
        verticalAlignment: Text.AlignVCenter
        leftPadding: 6
        rightPadding: 6
        background: Rectangle {
            color: "#12121c"
            border.color: "#2f3542"
        }
    }
    component PanelButton: Button {
        font.pixelSize: 11
        topPadding: 6
        bottomPadding: 6
        leftPadding: 8
        rightPadding: 8
    }

    function presentCurrentSlide() {
        const json = docs.currentSceneJson()
        if (json !== "") {
            worker.presentSceneJson(json)
        }
    }

    // Task 3.4: the reference's presenter keys. Escape clears all — and
    // restores when the output is already clear, matching the button's
    // Clear-all/Restore toggle. Disabled controls (nothing on air) simply
    // do nothing. On the edit page the first Escape steps out of the
    // selection first (4.3); the two owners gate on editCanvas.selected,
    // so the key never lands on both at once.
    Shortcut {
        sequence: StandardKey.Cancel
        enabled: (worker.wordsOnAir || worker.backgroundOnAir || worker.canRestore)
                 && !(page === "edit" && editCanvas.selected)
        onActivated: worker.canRestore ? worker.restoreOutput() : worker.clearAll()
    }
    Shortcut {
        sequence: "Escape"
        enabled: page === "edit" && editCanvas.selected
        onActivated: editCanvas.selected = false
    }
    Shortcut {
        sequence: "F1"
        enabled: worker.backgroundOnAir
        onActivated: worker.clearBackground()
    }
    Shortcut {
        sequence: "F2"
        enabled: worker.wordsOnAir
        onActivated: worker.clearSlide()
    }

    // Task 4.2: the drawer keys the reference carries (Ctrl+F opens its
    // search; Ctrl+D toggles the library strip unless a slide is current,
    // when it duplicates instead — the reference's selection rule, 4.5).
    // Ctrl+1..2 inside the drawer still pick its tabs — see LibraryDrawer
    // — but from the window they now pick pages, like the reference's
    // Ctrl+1..5 (4.3); 3..4 belong to pages that do not exist yet and do
    // nothing.
    Shortcut {
        sequence: "Ctrl+D"
        enabled: !typingIntoField && docs.slides.length > 0
        onActivated: docs.duplicateCurrentSlide()
    }
    Shortcut {
        sequence: "Ctrl+D"
        enabled: !typingIntoField && docs.slides.length === 0
        onActivated: panels.toggleDrawer()
    }
    Shortcut {
        sequence: "Ctrl+F"
        onActivated: libraryDrawer.focusSearch()
    }
    // The window's Ctrl+1..5 pick pages like the reference; 3..5 belong to
    // pages that do not exist yet, so those stay switched off. While the
    // keyboard is inside the drawer, Ctrl+1/2 stay with its tabs instead.
    Shortcut {
        sequence: "Ctrl+1"
        enabled: !libraryDrawer.containsFocus
        onActivated: root.page = "show"
    }
    Shortcut {
        sequence: "Ctrl+2"
        enabled: !libraryDrawer.containsFocus
        onActivated: root.page = "edit"
    }
    Shortcut {
        sequence: "Ctrl+3"
        enabled: false  // stage page arrives later
    }
    Shortcut {
        sequence: "Ctrl+4"
        enabled: false  // draw page arrives later
    }
    Shortcut {
        sequence: "Ctrl+5"
        enabled: !libraryDrawer.containsFocus
        onActivated: root.page = "settings"
    }

    // Task 4.3: the reference's nudge rule (EditTools.svelte:314-326) —
    // arrows walk the selected text box 1px, Ctrl+arrows 10px. Every step
    // is one undo entry, so it only runs on the edit page with a box
    // selected and nobody typing.
    function nudgeBox(dx, dy) {
        if (docs.currentSlide < 0 || docs.slides.length === 0) {
            return
        }
        const row = docs.slides[docs.currentSlide]
        docs.setSlideGeometry(row.x + dx, row.y + dy, row.width, row.height)
    }
    readonly property bool nudging: page === "edit" && editCanvas.selected
                                    && !typingIntoField && docs.slides.length > 0
    Shortcut {
        sequence: "Left"
        enabled: root.nudging
        onActivated: root.nudgeBox(-1, 0)
    }
    Shortcut {
        sequence: "Right"
        enabled: root.nudging
        onActivated: root.nudgeBox(1, 0)
    }
    Shortcut {
        sequence: "Up"
        enabled: root.nudging
        onActivated: root.nudgeBox(0, -1)
    }
    Shortcut {
        sequence: "Down"
        enabled: root.nudging
        onActivated: root.nudgeBox(0, 1)
    }
    Shortcut {
        sequence: "Ctrl+Left"
        enabled: root.nudging
        onActivated: root.nudgeBox(-10, 0)
    }
    Shortcut {
        sequence: "Ctrl+Right"
        enabled: root.nudging
        onActivated: root.nudgeBox(10, 0)
    }
    Shortcut {
        sequence: "Ctrl+Up"
        enabled: root.nudging
        onActivated: root.nudgeBox(0, -10)
    }
    Shortcut {
        sequence: "Ctrl+Down"
        enabled: root.nudging
        onActivated: root.nudgeBox(0, 10)
    }

    // Task 4.5: the reference's presenter keys (shortcuts.ts) — Space,
    // PageDown and Right advance, PageUp and Left step back, Home and End
    // jump, F5 advances for presenter clickers, "." clears all. They walk
    // the same navigation a Present click armed, so a service runs from
    // the keyboard alone. The gates are the reference's: typing keeps its
    // keys, a focused button or combo eats Space (AbstractButton is both),
    // the library drawer owns arrows for its own list, and the edit page's
    // selected box owns Left/Right — nudging and presenter arrows are
    // mutually exclusive, like the Escape pair.
    readonly property bool squattedFocus: activeFocusItem !== null
        && (activeFocusItem instanceof AbstractButton || activeFocusItem instanceof ComboBox)
    readonly property bool presenterTyping: typingIntoField
        || libraryDrawer.containsFocus || squattedFocus
    function presenterStep(delta) {
        if (docs.slides.length === 0) {
            return
        }
        docs.stepSlide(delta)
        root.presentCurrentSlide()
    }
    function presenterJump(index) {
        if (index >= 0 && index < docs.slides.length) {
            docs.selectSlide(index)
            root.presentCurrentSlide()
        }
    }
    readonly property bool presenterNav: !presenterTyping && docs.slides.length > 0
    Shortcut {
        sequence: "Space"
        enabled: root.presenterNav
        onActivated: root.presenterStep(1)
    }
    Shortcut {
        sequence: "PageDown"
        enabled: root.presenterNav
        onActivated: root.presenterStep(1)
    }
    Shortcut {
        sequence: "F5"
        enabled: root.presenterNav
        onActivated: root.presenterStep(1)
    }
    Shortcut {
        sequence: "PageUp"
        enabled: root.presenterNav
        onActivated: root.presenterStep(-1)
    }
    Shortcut {
        sequence: "Left"
        enabled: root.presenterNav && !root.nudging
        onActivated: root.presenterStep(-1)
    }
    Shortcut {
        sequence: "Right"
        enabled: root.presenterNav && !root.nudging
        onActivated: root.presenterStep(1)
    }
    Shortcut {
        sequence: "Home"
        enabled: root.presenterNav
        onActivated: root.presenterJump(0)
    }
    Shortcut {
        sequence: "End"
        enabled: root.presenterNav
        onActivated: root.presenterJump(docs.slides.length - 1)
    }
    Shortcut {
        sequence: "."
        enabled: root.presenterNav && (worker.wordsOnAir || worker.backgroundOnAir)
        onActivated: worker.clearAll()
    }

    // Task 4.5: the core document keys. While a field has the focus the
    // editing keys belong to it — undo, redo and the clipboard letters
    // all have native text-field meanings — so they are gated away from
    // typing. Ctrl+S is not a keystroke any field wants, so saving works
    // at any time (it flushes the pending edit first, like every other
    // controller call). The clipboard is the controller's slide clipboard
    // — the system one stays with the text fields, exactly as in the
    // reference.
    Shortcut {
        sequence: StandardKey.Undo
        enabled: !typingIntoField
        onActivated: docs.undo()
    }
    Shortcut {
        sequence: "Ctrl+Y"
        enabled: !typingIntoField
        onActivated: docs.redo()
    }
    Shortcut {
        sequence: "Ctrl+Shift+Z"
        enabled: !typingIntoField
        onActivated: docs.redo()
    }
    Shortcut {
        sequence: StandardKey.Save
        onActivated: docs.save()
    }
    // Explicit letters rather than StandardKey.Copy/Cut/Paste: the
    // standard keys also carry Ctrl+Ins-style bindings that this app has
    // no use for, and each one logs a binding warning at startup.
    Shortcut {
        sequence: "Ctrl+C"
        enabled: !typingIntoField
        onActivated: docs.copyCurrentSlide()
    }
    Shortcut {
        sequence: "Ctrl+X"
        enabled: !typingIntoField
        onActivated: docs.cutCurrentSlide()
    }
    Shortcut {
        sequence: "Ctrl+V"
        enabled: !typingIntoField
        onActivated: docs.pasteSlide()
    }

    // Presenter number keys, the reference's opt-in: 1..9 play slide
    // one..nine and 0 plays ten. Off until the settings page enables it,
    // and silent while typing, inside the drawer, or on the edit page.
    readonly property bool numberKeys: appSettings.numberKeysPresent
        && !presenterTyping && page !== "edit" && docs.slides.length > 0
    Shortcut {
        sequence: "1"
        enabled: root.numberKeys
        onActivated: root.presenterJump(0)
    }
    Shortcut {
        sequence: "2"
        enabled: root.numberKeys
        onActivated: root.presenterJump(1)
    }
    Shortcut {
        sequence: "3"
        enabled: root.numberKeys
        onActivated: root.presenterJump(2)
    }
    Shortcut {
        sequence: "4"
        enabled: root.numberKeys
        onActivated: root.presenterJump(3)
    }
    Shortcut {
        sequence: "5"
        enabled: root.numberKeys
        onActivated: root.presenterJump(4)
    }
    Shortcut {
        sequence: "6"
        enabled: root.numberKeys
        onActivated: root.presenterJump(5)
    }
    Shortcut {
        sequence: "7"
        enabled: root.numberKeys
        onActivated: root.presenterJump(6)
    }
    Shortcut {
        sequence: "8"
        enabled: root.numberKeys
        onActivated: root.presenterJump(7)
    }
    Shortcut {
        sequence: "9"
        enabled: root.numberKeys
        onActivated: root.presenterJump(8)
    }
    Shortcut {
        sequence: "0"
        enabled: root.numberKeys
        onActivated: root.presenterJump(9)
    }

    // F11 flips the window between fullscreen and windowed (the
    // reference's app fullscreen) and is remembered, so a projector
    // setup survives restarts.
    Shortcut {
        sequence: "F11"
        onActivated: appSettings.fullscreen = !appSettings.fullscreen
    }

    TopBar {
        id: topBar
        page: root.page
        onPageSelected: name => root.page = name
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
    }

    /* ------------------------------------------------------------------
     * Left panel — the show page of the reference: the playlist projects
     * on top (with the current one) and the running order itself below.
     * Rows reorder by drag or the small handles (drop targets compute
     * the gap), rows arriving from the drawer insert at the drop point,
     * and everything goes through the undoable controller (§5).
     * ------------------------------------------------------------------ */
    SidePanel {
        id: leftDock
        side: "left"
        anchors.left: parent.left
        anchors.top: topBar.bottom
        anchors.bottom: libraryDrawer.top
        width: panels.leftWidth
        onDraggedTo: candidate => panels.resizeLeft(candidate)
        onStripClicked: panels.clickLeft()

        Item {
            anchors.fill: parent
            anchors.margins: 10

            Text {
                id: playlistsTitle
                anchors.top: parent.top
                anchors.left: parent.left
                color: "#8f93a5"
                font.pixelSize: 11
                font.bold: true
                text: "PLAYLISTS"
            }

            Row {
                id: playlistRow
                anchors.top: playlistsTitle.bottom
                anchors.topMargin: 4
                anchors.left: parent.left
                anchors.right: parent.right
                spacing: 6

                ListView {
                    id: playlistList
                    orientation: ListView.Horizontal
                    width: playlistRow.width - newPlaylistButton.width - 6
                    height: 26
                    spacing: 6
                    clip: true
                    model: docs.playlists

                    delegate: Rectangle {
                        id: playlistChip

                        required property var modelData
                        width: chipText.width + 18
                        height: 26
                        radius: 4
                        color: playlistChip.modelData.id === docs.currentPlaylistId ? "#f0008c" : "#12121c"
                        border.color: "#2f3542"

                        Text {
                            id: chipText
                            anchors.centerIn: parent
                            font.pixelSize: 12
                            color: playlistChip.modelData.id === docs.currentPlaylistId ? "#ffffff" : "#f0f0ff"
                            text: playlistChip.modelData.name
                        }

                        MouseArea {
                            anchors.fill: parent
                            onClicked: docs.selectPlaylist(playlistChip.modelData.id)
                        }
                    }
                }

                // Chip-styled (the Controls Button rendered its "+" pill
                // without the glyph at this size).
                Rectangle {
                    id: newPlaylistButton

                    width: 32
                    height: 26
                    radius: 4
                    color: newPlaylistMouse.containsMouse ? "#2f3542" : "#12121c"
                    border.color: "#2f3542"

                    Text {
                        anchors.centerIn: parent
                        font.pixelSize: 15
                        color: "#f0f0ff"
                        text: "+"
                    }

                    MouseArea {
                        id: newPlaylistMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: docs.createPlaylist("Playlist " + (docs.playlists.length + 1))
                    }
                }
            }

            Text {
                id: playlistTitle
                anchors.top: playlistRow.bottom
                anchors.topMargin: 10
                anchors.left: parent.left
                color: "#8f93a5"
                font.pixelSize: 11
                font.bold: true
                text: "PLAYLIST"
            }

            Rectangle {
                id: playlistBox
                anchors.top: playlistTitle.bottom
                anchors.topMargin: 6
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: serviceDirText.top
                anchors.bottomMargin: 8
                color: "#12121c"
                border.color: "#2f3542"

                ListView {
                    id: entryList
                    anchors.fill: parent
                    anchors.margins: 1
                    clip: true
                    model: docs.playlistEntries

                    delegate: Item {
                        id: entryRow

                        required property var modelData
                        required property int index
                        width: entryList.width
                        height: 30

                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: 1
                            color: entryRow.modelData.showId === docs.currentSongId ? "#f0008c"
                                 : (entryMouse.containsMouse ? "#2f3542" : "transparent")
                        }

                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: 6
                            anchors.verticalCenter: parent.verticalCenter
                            width: entryRow.width - 76
                            elide: Text.ElideRight
                            font.pixelSize: 13
                            color: entryRow.modelData.showId === docs.currentSongId ? "#ffffff" : "#f0f0ff"
                            text: (entryRow.modelData.section !== ""
                                   ? entryRow.modelData.section + " \u00b7 " : "")
                                  + entryRow.modelData.showName
                        }

                        // The reorder handles: the drag is the reference's
                        // path, these make the same move without a mouse.
                        Row {
                            anchors.right: parent.right
                            anchors.rightMargin: 2
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 2

                            Repeater {
                                model: [
                                    {glyph: "\u25b2", enabled: entryRow.index > 0,
                                     step: -1, action: "move"},
                                    {glyph: "\u25bc",
                                     enabled: entryRow.index < entryList.count - 1,
                                     step: 1, action: "move"},
                                    {glyph: "\u2715", enabled: true, step: 0, action: "remove"}
                                ]

                                delegate: Text {
                                    id: handle

                                    required property var modelData
                                    width: 20
                                    height: 24
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                    font.pixelSize: modelData.action === "remove" ? 11 : 9
                                    color: !modelData.enabled ? "#3a3f4e"
                                         : (handleMouse.containsMouse ? "#f0f0ff" : "#8f93a5")
                                    text: modelData.glyph

                                    MouseArea {
                                        id: handleMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        enabled: handle.modelData.enabled
                                        onClicked: {
                                            if (handle.modelData.action === "move") {
                                                docs.movePlaylistEntry(
                                                    entryRow.modelData.entryId,
                                                    entryRow.index + handle.modelData.step)
                                            } else {
                                                docs.removePlaylistEntry(entryRow.modelData.entryId)
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        MouseArea {
                            id: entryMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            drag.target: entryRow
                            onClicked: docs.selectSong(entryRow.modelData.showId)
                            onReleased: if (entryRow.Drag.active) entryRow.Drag.drop()
                        }

                        Drag.active: entryMouse.drag.active
                        Drag.keys: ["text/plain"]
                        Drag.mimeData: ({ "text/plain": "entry:" + entryRow.modelData.entryId })
                        Drag.hotSpot: Qt.point(width / 2, height / 2)
                    }

                    Text {
                        anchors.centerIn: parent
                        visible: entryList.count === 0
                        color: "#6a6f80"
                        font.pixelSize: 13
                        text: "Nothing here"  // the reference's empty playlist
                    }
                }

                // The drop target: drops land in the gap nearest the
                // pointer (rows are 30 high), shows insert and entries
                // move around the row being dragged.
                DropArea {
                    anchors.fill: parent
                    keys: ["text/plain"]

                    onDropped: function (drop) {
                        const parts = drop.mimeData.text.split(":")
                        const kind = parts[0]
                        const id = parts[1]
                        if (!id) {
                            return
                        }
                        // Round to the nearest row boundary: above the
                        // middle of row b means index b, below means b+1.
                        const index = Math.round((drop.y - entryList.y) / 30)
                        if (kind === "show") {
                            docs.insertShowInPlaylist(id, index)
                            drop.accept()
                        } else if (kind === "entry") {
                            // toIndex is the position after the move, so a
                            // row moving downwards past the gap shifts by one.
                            const entries = docs.playlistEntries
                            let from = -1
                            for (let i = 0; i < entries.length; ++i) {
                                if (entries[i].entryId === id) {
                                    from = i
                                    break
                                }
                            }
                            docs.movePlaylistEntry(id, from >= 0 && from < index ? index - 1 : index)
                            drop.accept()
                        }
                    }
                }
            }

            Text {
                id: serviceDirText
                anchors.bottom: parent.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                wrapMode: Text.WordWrap
                color: docs.dirty ? "#d8c07a" : "#8f93a5"
                font.pixelSize: 11
                text: (docs.dirty ? "\u25cf unsaved  \u00b7  " : "")
                      + (docs.status !== "" ? docs.status + "  \u00b7  " : "")
                      + docs.serviceDirectory
            }
        }
    }

    /* ------------------------------------------------------------------
     * Right panel — always the preview, with the show's output tools
     * below it (the reference's Preview + ShowTools arrangement).
     * ------------------------------------------------------------------ */
    SidePanel {
        id: rightDock
        side: "right"
        anchors.right: parent.right
        anchors.top: topBar.bottom
        anchors.bottom: libraryDrawer.top
        width: panels.rightWidth
        onDraggedTo: candidate => panels.resizeRight(candidate)
        onStripClicked: panels.clickRight()

        Item {
            anchors.fill: parent
            anchors.margins: 10

            // Task 1.4: the audience output as the worker sees it, streamed
            // over shared memory. Qt only paints bytes here; the scene is
            // the worker's Skia render, downscaled to preview size.
            Rectangle {
                id: previewBox
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                height: width * 9 / 16
                visible: worker.outputLive
                color: "black"
                border.color: "#2f3542"

                PreviewView {
                    anchors.fill: parent
                    anchors.margins: 1
                    controller: worker
                }
            }

            Text {
                id: previewStats
                anchors.top: previewBox.bottom
                anchors.topMargin: 4
                anchors.left: parent.left
                visible: worker.outputLive
                color: "#8f93a5"
                font.pixelSize: 11
                text: worker.previewActive
                    ? "preview \u00b7 " + worker.previewFrames + " frames shown \u00b7 "
                      + worker.previewDropped + " dropped"
                    : (worker.previewError !== ""
                       ? "preview: " + worker.previewError : "preview starting...")
            }

            Column {
                id: showTools
                anchors.top: previewStats.bottom
                anchors.topMargin: 12
                anchors.left: parent.left
                anchors.right: parent.right
                spacing: 10
                visible: worker.outputLive

                Row {
                    spacing: 8

                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        color: "#8f93a5"
                        font.pixelSize: 12
                        text: "Output:"
                    }

                    ComboBox {
                        id: destination
                        width: showTools.width - 70
                        model: [{value: -1, label: "Windowed"}].concat(
                            worker.displays.map(d => ({value: d.index,
                                                       label: d.name + " — " + d.size})))
                        textRole: "label"
                        valueRole: "value"
                        // Bound to the worker's applied state, so the picker
                        // shows where the output actually is until a switch
                        // confirms.
                        currentIndex: destination.indexOfValue(worker.currentMonitor)
                        onActivated: worker.selectDisplay(currentValue)
                    }
                }

                Flow {
                    width: parent.width
                    spacing: 10

                    Repeater {
                        model: ["background", "images", "words", "aux", "clock"]

                        delegate: CheckBox {
                            required property string modelData
                            text: modelData
                            checked: worker.layers[modelData] === true
                            onToggled: worker.setLayerEnabled(modelData, checked)
                        }
                    }
                }

                // Task 2.4c: compiled demo scenes through the same path real
                // content takes. The label shows the worker's store.
                Flow {
                    width: parent.width
                    spacing: 8
                    visible: worker.demoSlideCount > 0

                    Repeater {
                        model: worker.demoSlideCount

                        Button {
                            required property int index
                            text: "scene " + (index + 1)
                            onClicked: worker.presentDemoSlide(index)
                        }
                    }

                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        color: "#8f93a5"
                        font.pixelSize: 11
                        text: "scene " + worker.sceneActive
                              + (worker.scenePending !== worker.sceneActive
                                 ? " \u2192 " + worker.scenePending : "")
                              + " \u00b7 " + worker.scenePrepared + " prepared"
                    }
                }

                Rectangle {
                    width: parent.width
                    height: 1
                    color: "#2f3542"
                }

                // Task 3.4: output controls. Enabled state and live flags
                // come from the worker's acknowledged active scene, never
                // from a click: each clear is disabled while its own layer
                // is off air (the reference's "independent per-layer
                // clearing made visible"), and Clear all becomes Restore
                // output once the output is clear.
                Flow {
                    width: parent.width
                    spacing: 8

                    Button {
                        text: "Clear slide"
                        enabled: worker.wordsOnAir
                        onClicked: worker.clearSlide()
                    }

                    Button {
                        text: "Clear background"
                        enabled: worker.backgroundOnAir
                        onClicked: worker.clearBackground()
                    }

                    Button {
                        text: worker.canRestore ? "Restore output" : "Clear all"
                        enabled: worker.wordsOnAir || worker.backgroundOnAir || worker.canRestore
                        onClicked: worker.canRestore ? worker.restoreOutput() : worker.clearAll()
                    }
                }

                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    color: "#8f93a5"
                    font.pixelSize: 11
                    text: {
                        const onAir = []
                        if (worker.backgroundOnAir) onAir.push("background")
                        if (worker.wordsOnAir) onAir.push("words")
                        return onAir.length > 0 ? "on air: " + onAir.join(" + ") : "output clear"
                    }
                }

                // Task 6.15a (review R15): the media transport, live over
                // the socket. Every control mirrors worker status — the
                // Pause/Play label follows mediaPlaying, the sliders
                // follow what the worker last reported — so a refused
                // seek snaps the thumb back to where the media actually
                // is, and a loop toggle shows the intent the end of the
                // clip will honour, not the moment it was clicked.
                // Task 6.15b: the transport row belongs to the background
                // video; the mixer rows below list whatever sources are
                // registered across BOTH transports, so the column shows
                // whenever either one is on air.
                Column {
                    width: parent.width
                    spacing: 6
                    visible: worker.mediaOnAir || worker.standaloneOnAir

                    Flow {
                        width: parent.width
                        spacing: 8
                        visible: worker.mediaOnAir

                        Button {
                            text: worker.mediaPlaying ? "Pause" : "Play"
                            onClicked: worker.mediaPlaying ? worker.mediaPause()
                                                           : worker.mediaPlay()
                        }

                        CheckBox {
                            text: "Loop"
                            checked: worker.mediaLoop
                            onToggled: worker.setMediaLoop(checked)
                        }

                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            color: "#8f93a5"
                            font.pixelSize: 11
                            text: worker.mediaPosition >= 0
                                  ? worker.mediaPosition.toFixed(1) + " s"
                                        + (worker.mediaDuration > 0
                                           ? " / " + worker.mediaDuration.toFixed(1) + " s"
                                           : "")
                                  : "\u2014"
                        }
                    }

                    // Scrub only where there is a scale to scrub on: a
                    // container that will not say its length gets the
                    // readout above, not a slider with no right end. The
                    // value is re-applied from status only while the
                    // thumb is at rest, so a 500 ms poll cannot yank a
                    // drag out from under the finger; every move rides the
                    // transport's own newest-wins coalescing.
                    Slider {
                        id: seekSlider
                        width: parent.width
                        visible: worker.mediaDuration > 0
                        from: 0
                        to: Math.max(worker.mediaDuration, 0.0001)
                        onMoved: if (active) worker.mediaSeek(value)
                        Connections {
                            target: worker
                            function onMediaChanged() {
                                if (!seekSlider.pressed && !seekSlider.dragging) {
                                    seekSlider.value = worker.mediaPosition
                                }
                            }
                        }
                    }

                    Repeater {
                        model: worker.mediaSources

                        delegate: Row {
                            id: sourceRow
                            required property var modelData
                            spacing: 6

                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                color: "#8f93a5"
                                font.pixelSize: 11
                                text: modelData.id
                            }

                            Slider {
                                id: gainSlider
                                width: 120
                                from: 0
                                to: 1
                                value: modelData.gain
                                onMoved: if (active) worker.setMediaGain(modelData.id, value)
                                Connections {
                                    target: worker
                                    function onMediaChanged() {
                                        if (!gainSlider.pressed && !gainSlider.dragging) {
                                            gainSlider.value = sourceRow.modelData.gain
                                        }
                                    }
                                }
                            }

                            CheckBox {
                                text: "mute"
                                checked: modelData.muted
                                onToggled: worker.setMediaMute(modelData.id, checked)
                            }
                        }
                    }
                }

                // Task 6.15b (review R15): the standalone production track.
                // It keeps its OWN transport, so lyric changes, background-
                // video replacements and even the background going away
                // never restart it — which is also why its controls sit
                // outside the transport column above: they address a
                // different lifetime. The status line below is the worker's
                // report, not a hopeful selection; a refused path surfaces
                // on worker.status like any other refused command.
                Row {
                    width: parent.width
                    spacing: 6

                    PanelField {
                        id: standaloneField
                        width: 200
                        placeholderText: "standalone track path"
                    }

                    PanelButton {
                        text: "Set track"
                        enabled: standaloneField.text.trim().length > 0
                                 && worker.outputLive
                        onClicked: worker.selectStandaloneTrack(standaloneField.text.trim())
                    }

                    PanelButton {
                        text: "Remove track"
                        enabled: worker.standaloneOnAir
                        onClicked: worker.selectStandaloneTrack("")
                    }
                }

                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    font.pixelSize: 11
                    color: worker.standaloneError.length > 0 ? "#d9a441" : "#8f93a5"
                    text: {
                        if (worker.standaloneError.length > 0) {
                            return worker.standaloneError
                        }
                        if (!worker.standaloneOnAir) {
                            return "no standalone track"
                        }
                        return "standalone \u00b7 " + worker.standalonePath
                               + (worker.standalonePosition >= 0
                                  ? " \u00b7 " + worker.standalonePosition.toFixed(1) + " s"
                                  : "")
                    }
                }
            }

            Text {
                anchors.bottom: parent.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                visible: worker.outputLive
                wrapMode: Text.WordWrap
                color: "#8f93a5"
                font.pixelSize: 11
                // The panel name is looked up in displays, guarded: status
                // can arrive before the display list (or after a hotplug
                // empties it).
                text: {
                    const panel = worker.currentMonitor >= 0
                        ? worker.displays[worker.currentMonitor] : null
                    const where = worker.currentMonitor < 0 ? "windowed"
                        : (panel ? panel.name : "monitor " + worker.currentMonitor)
                    return worker.device + " \u00b7 frame " + worker.frames
                           + " \u00b7 revision " + worker.revision + " \u00b7 " + where
                }
            }
        }
    }

    /* ------------------------------------------------------------------
     * Center — two pages (4.3) on the reference's show/edit split. The
     * show page: the current song's slide list and the lyric editor.
     * Everything here drives DocumentController ("docs"); presenting
     * compiles the selected slide and hands the JSON to the worker,
     * which presents on the acknowledgement.
     * ------------------------------------------------------------------ */
    Item {
        id: showPage
        visible: root.page === "show"
        anchors.left: leftDock.right
        anchors.right: rightDock.left
        anchors.top: topBar.bottom
        anchors.bottom: libraryDrawer.top
        anchors.margins: 12

        Row {
            id: slideActions
            anchors.top: parent.top
            anchors.left: parent.left
            spacing: 8

            Button {
                text: "Add slide"
                enabled: docs.currentSongId !== ""
                onClicked: docs.addSlide()
            }

            Button {
                text: "Remove slide"
                enabled: docs.slides.length > 0
                onClicked: docs.removeCurrentSlide()
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                color: "#8f93a5"
                font.pixelSize: 12
                text: docs.slides.length > 0
                    ? "slide " + (docs.currentSlide + 1) + " of " + docs.slides.length
                    : "no slides"
            }
        }

        // The slide list's pictures (task 6.16b, review R16): the worker
        // renders the visible band with the production compositor, so a row
        // shows the slide as the audience would see it — same fonts, same
        // wrap, same background — instead of a line of Qt-laid-out text.
        //
        // The band is the current slide ±5, which is the rows on screen (the
        // box fits about six at 26 px) plus a scroll's worth either way, and
        // it is inside the 32-slide cap the protocol declares. Asking per
        // visible row would send one batch per pixel of scroll; asking for a
        // band moves the list in steps, and the client coalesces a band it
        // already has. A slide the worker has not drawn yet keeps its text
        // line: the fallback is the row's own, so a list is never blank while
        // pictures are on their way.
        function requestSlideBand() {
            if (!worker.thumbnailSupported) {
                return
            }
            const slides = docs.slides
            if (slides.length === 0) {
                worker.clearThumbnails()
                return
            }
            const band = []
            const first = Math.max(0, docs.currentSlide - 5)
            const last = Math.min(slides.length - 1, docs.currentSlide + 5)
            for (let i = first; i <= last; ++i) {
                band.push({key: slides[i].id,
                           scene: docs.sceneJsonForSlide(slides[i].id)})
            }
            worker.requestThumbnails(band, 160, 90)
        }

        // Navigating, editing and editing the document (a slide added, a song
        // switched) all change what the list wants a picture of. The client
        // re-issues across a preview restart and a worker that dropped the
        // batch on its own, so the list does not have to watch the socket too.
        Connections {
            target: docs
            function onNavigationChanged() { showPage.requestSlideBand() }
            function onDocumentsChanged() { showPage.requestSlideBand() }
            function onSlideTextChanged() { showPage.requestSlideBand() }
        }
        // The capability arrives with the handshake, which is usually after
        // this page was built. ONE ask follows the first moment the worker can
        // draw the list: previewChanged fires with every preview frame, and a
        // handler that asked each time would recompile eleven scenes fifteen
        // times a second for a list that already has them.
        property bool bandAskedFor: false
        Connections {
            target: worker
            function onPreviewChanged() {
                if (worker.thumbnailSupported && !showPage.bandAskedFor) {
                    showPage.bandAskedFor = true
                    showPage.requestSlideBand()
                }
            }
        }
        Component.onCompleted: Qt.callLater(function() { showPage.requestSlideBand() })

        Rectangle {
            id: slideListBox
            anchors.top: slideActions.bottom
            anchors.topMargin: 8
            anchors.left: parent.left
            anchors.right: parent.right
            height: 150
            color: "#12121c"
            border.color: "#2f3542"

            ListView {
                id: slideList
                anchors.fill: parent
                anchors.margins: 1
                clip: true
                model: docs.slides

                delegate: ItemDelegate {
                    id: control
                    required property var modelData
                    width: slideList.width
                    height: 26
                    text: (modelData.index + 1) + ".  " + modelData.preview
                    highlighted: modelData.index === docs.currentSlide
                    contentItem: Row {
                        spacing: 6
                        // The worker's own picture of this slide (task 6.16b).
                        // It is an addition, not a replacement: the text line
                        // stays, so a row is legible before its picture
                        // arrives, on a worker that cannot draw the list, and
                        // to a screen reader either way.
                        PreviewView {
                            id: rowPicture
                            visible: worker.thumbnailSupported
                            width: 46
                            height: 26
                            anchors.verticalCenter: parent.verticalCenter
                            controller: worker
                            target: PreviewView.Thumbnail
                            key: modelData.id
                        }
                        Text {
                            // The picture's 46 px only when there is one: a
                            // positioner lays out nothing for an invisible
                            // child, so narrowing the line for a picture that
                            // is not there would elide it for no reason.
                            width: slideList.width
                                   - (rowPicture.visible ? rowPicture.width : 0) - 40
                            text: control.text
                            color: control.highlighted ? "#ffffff" : "#f0f0ff"
                            font.pixelSize: 13
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                        }
                    }
                    background: Rectangle {
                        color: control.highlighted ? "#f0008c"
                             : (control.hovered ? "#2f3542" : "transparent")
                    }
                    onClicked: {
                        docs.selectSlide(modelData.index)
                        if (root.following) {
                            root.presentCurrentSlide()
                        }
                    }
                }
            }
        }

        Row {
            id: historyRow
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            spacing: 8

            Button {
                text: "Undo"
                enabled: docs.canUndo
                onClicked: docs.undo()
            }

            Button {
                text: "Redo"
                enabled: docs.canRedo
                onClicked: docs.redo()
            }

            Button {
                text: "Save"
                enabled: docs.dirty
                onClicked: docs.save()
            }

            Button {
                text: "Load"
                onClicked: docs.load()
            }
        }

        Row {
            id: navRow
            anchors.bottom: historyRow.top
            anchors.bottomMargin: 10
            anchors.left: parent.left
            spacing: 8

            Button {
                text: "\u25c0 Prev"
                enabled: docs.slides.length > 0
                onClicked: {
                    docs.stepSlide(-1)
                    if (root.following) {
                        root.presentCurrentSlide()
                    }
                }
            }

            Button {
                text: "Next \u25b6"
                enabled: docs.slides.length > 0
                onClicked: {
                    docs.stepSlide(1)
                    if (root.following) {
                        root.presentCurrentSlide()
                    }
                }
            }

            Button {
                text: "Present"
                enabled: docs.slides.length > 0 && worker.outputLive
                onClicked: {
                    root.following = true
                    root.presentCurrentSlide()
                }
            }

            CheckBox {
                text: "follow"
                checked: root.following
                onToggled: root.following = checked
            }
        }

        // The editor writes into the debounced controller slot; the
        // binding is re-established (not just assigned) when navigation,
        // undo or a load changes the slide under the operator, so future
        // changes keep flowing without breaking typing again.
        TextArea {
            id: lyricText
            anchors.top: slideListBox.bottom
            anchors.topMargin: 10
            anchors.bottom: navRow.top
            anchors.bottomMargin: 10
            anchors.left: parent.left
            anchors.right: parent.right
            text: docs.currentSlideText
            onTextChanged: docs.setSlideText(text)
            selectByMouse: true
            wrapMode: TextArea.Wrap
            color: "#f0f0ff"
            background: Rectangle {
                color: "#12121c"
                border.color: "#2f3542"
            }

            Connections {
                target: docs
                function onSlideTextChanged() {
                    lyricText.text = Qt.binding(function () { return docs.currentSlideText })
                    lyricText.cursorPosition = 0
                }
            }
        }
    }

    /* ------------------------------------------------------------------
     * The edit page (task 4.3): the current slide's text box on a 16:9
     * stage — drag to move, corners to resize, arrows to nudge — with
     * exact X/Y/W/H fields under it as the deterministic path. One undo
     * entry per drag, per nudge, and per field commit.
     * ------------------------------------------------------------------ */
    Item {
        id: editPage
        visible: root.page === "edit"
        anchors.left: leftDock.right
        anchors.right: rightDock.left
        anchors.top: topBar.bottom
        anchors.bottom: libraryDrawer.top
        anchors.margins: 12

        EditCanvas {
            id: editCanvas
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: stylePanel.left
            anchors.rightMargin: 12
            anchors.bottom: boxFields.top
            anchors.bottomMargin: 12
        }

        /*
         * Asking the worker for its picture of the slide under edit (task
         * 6.16a, review R16). Debounced: one ask costs a scene.prepare and an
         * off-air draw, so a keystroke at a time would be renders nobody has
         * looked at yet. The triggers below are everything that can change
         * what the canvas should show — text, style, geometry (a drag
         * commits on release, which is a document change), navigation and
         * page entry — and the payload is compiled at the tick, so the
         * render is of the slide as it ended up rather than as it was when
         * typing started.
         */
        Timer {
            id: selectionAsk
            interval: 250
            repeat: false
            onTriggered: {
                if (!editPage.visible || !editCanvas.hasSlide) {
                    worker.clearSelectionPreview()
                    return
                }
                worker.requestSelectionPreview(
                    docs.sceneJsonForSlide(docs.slides[docs.currentSlide].id))
            }
        }
        function askForWorkerPicture() {
            selectionAsk.restart()
        }
        Connections {
            target: docs
            function onDocumentsChanged() {
                if (editPage.visible) {
                    editPage.askForWorkerPicture()
                }
            }
            function onSlideTextChanged() {
                if (editPage.visible) {
                    editPage.askForWorkerPicture()
                }
            }
            function onNavigationChanged() {
                if (editPage.visible) {
                    editPage.askForWorkerPicture()
                }
            }
        }
        // Leaving the page stands the target down: the worker stops drawing
        // (and holding a scene for) a slide nobody is looking at, and the
        // stream goes back to mirroring the audience alone.
        onVisibleChanged: {
            if (editPage.visible) {
                editPage.askForWorkerPicture()
            } else {
                selectionAsk.stop()
                worker.clearSelectionPreview()
            }
        }

        Row {
            id: boxFields
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            spacing: 8

            // The fields show the live box; the one that loses focus
            // commits all four. A value the domain refuses (or that is
            // not a number at all) snaps every field back.
            function commit() {
                const x = parseFloat(fieldX.text)
                const y = parseFloat(fieldY.text)
                const w = parseFloat(fieldW.text)
                const h = parseFloat(fieldH.text)
                if (!Number.isFinite(x) || !Number.isFinite(y)
                        || !(w >= 1) || !(h >= 1)
                        || !docs.setSlideGeometry(x, y, w, h)) {
                    boxFields.sync()
                }
            }
            function sync() {
                fieldX.text = Qt.binding(function () { return editCanvas.boxX.toFixed(0) })
                fieldY.text = Qt.binding(function () { return editCanvas.boxY.toFixed(0) })
                fieldW.text = Qt.binding(function () { return editCanvas.boxW.toFixed(0) })
                fieldH.text = Qt.binding(function () { return editCanvas.boxH.toFixed(0) })
            }
            Component.onCompleted: sync()
            Connections {
                target: docs
                function onDocumentsChanged() { boxFields.sync() }
                function onNavigationChanged() { boxFields.sync() }
            }
            Connections {
                target: editPage
                function onVisibleChanged() { if (editPage.visible) boxFields.sync() }
            }

            Column {
                spacing: 2
                Text {
                    color: "#8f93a5"
                    font.pixelSize: 10
                    text: "X"
                }
                TextField {
                    id: fieldX
                    objectName: "geometryField"
                    width: 72
                    color: "#f0f0ff"
                    font.pixelSize: 13
                    onEditingFinished: boxFields.commit()
                    background: Rectangle {
                        color: "#12121c"
                        border.color: "#2f3542"
                    }
                }
            }

            Column {
                spacing: 2
                Text {
                    color: "#8f93a5"
                    font.pixelSize: 10
                    text: "Y"
                }
                TextField {
                    id: fieldY
                    objectName: "geometryField"
                    width: 72
                    color: "#f0f0ff"
                    font.pixelSize: 13
                    onEditingFinished: boxFields.commit()
                    background: Rectangle {
                        color: "#12121c"
                        border.color: "#2f3542"
                    }
                }
            }

            Column {
                spacing: 2
                Text {
                    color: "#8f93a5"
                    font.pixelSize: 10
                    text: "W"
                }
                TextField {
                    id: fieldW
                    objectName: "geometryField"
                    width: 72
                    color: "#f0f0ff"
                    font.pixelSize: 13
                    onEditingFinished: boxFields.commit()
                    background: Rectangle {
                        color: "#12121c"
                        border.color: "#2f3542"
                    }
                }
            }

            Column {
                spacing: 2
                Text {
                    color: "#8f93a5"
                    font.pixelSize: 10
                    text: "H"
                }
                TextField {
                    id: fieldH
                    objectName: "geometryField"
                    width: 72
                    color: "#f0f0ff"
                    font.pixelSize: 13
                    onEditingFinished: boxFields.commit()
                    background: Rectangle {
                        color: "#12121c"
                        border.color: "#2f3542"
                    }
                }
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                color: "#6a6f80"
                font.pixelSize: 11
                text: "drag the box · corners resize · arrows nudge 1px, Ctrl 10px"
            }
        }

        /*
         * The style panel (task 4.4): one style object is under edit —
         * the one the current slide resolves through, override or show
         * default. Editing it changes every slide that follows it, which
         * is the reuse the reference gives its themes; New forks the
         * look for this slide alone, Show default hands it to every
         * un-overridden slide, and Follow show (the empty id) gives the
         * override back. Fields commit on focus-out and snap back on a
         * refusal, exactly like the box fields beside them.
         */
        Column {
            id: stylePanel
            objectName: "stylePanel"
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            width: 250
            spacing: 10

            // The style the fields edit right now ("" = none anywhere).
            function targetId() {
                return docs.currentStyle.id || ""
            }
            // Combo row 0 is "follow show" (the empty id); the rest name
            // the styles view's rows in order.
            function styleIdAt(index) {
                if (index <= 0) {
                    return ""
                }
                var i = index - 1
                return i < docs.styles.length ? docs.styles[i].id : ""
            }
            // Name, colours, font and typography commit as one edit.
            // A slide following nothing creates its style on first edit.
            function commit(newAlign) {
                const size = parseFloat(fieldStyleSize.text)
                const spacing = parseFloat(fieldLineSpacing.text)
                const align = newAlign !== undefined ? newAlign
                                                     : (docs.currentStyle.align || "center")
                const name = fieldStyleName.text.trim() || "Style"
                const colour = fieldTextColor.text.trim()
                const back = fieldBackColour.text.trim()
                const family = fieldFontFamily.text.trim()
                const spacingValue = Number.isFinite(spacing) ? spacing : 1.25
                const sizeValue = Number.isFinite(size) && size >= 0 ? size : 0
                if (targetId() === "") {
                    if (!docs.createStyleFromCurrent(name)) {
                        stylePanel.sync()
                        return
                    }
                    // The creation took the built-in look; carry the
                    // operator's values onto it, undoing the creation too
                    // if the domain refuses them.
                    if (!docs.editStyle(targetId(), name, colour, back, family,
                                        sizeValue, align, spacingValue)) {
                        docs.undo()
                        stylePanel.sync()
                    }
                    return
                }
                if (!docs.editStyle(targetId(), name, colour, back, family,
                                    sizeValue, align, spacingValue)) {
                    stylePanel.sync()
                }
            }
            function sync() {
                fieldStyleName.text = Qt.binding(function () {
                    return docs.currentStyle.name || ""
                })
                fieldTextColor.text = Qt.binding(function () {
                    return docs.currentStyle.textColor || "#ffffff"
                })
                fieldBackColour.text = Qt.binding(function () {
                    return docs.currentStyle.backgroundColor || "#000000"
                })
                fieldFontFamily.text = Qt.binding(function () {
                    return docs.currentStyle.fontFamily || ""
                })
                fieldStyleSize.text = Qt.binding(function () {
                    const size = docs.currentStyle.fontSize
                    return size === undefined ? "0" : String(size)
                })
                fieldLineSpacing.text = Qt.binding(function () {
                    const spacing = docs.currentStyle.lineSpacing
                    return spacing === undefined ? "1.25" : String(spacing)
                })
                // The combo names the followed default as its first row;
                // an override selects its own style instead.
                var index = 0
                if (docs.currentStyleIsOverride) {
                    for (var i = 0; i < docs.styles.length; ++i) {
                        if (docs.styles[i].id === stylePanel.targetId()) {
                            index = i + 1
                            break
                        }
                    }
                }
                styleCombo.currentIndex = index
                var background = ""
                if (docs.currentSlide >= 0 && docs.slides.length > docs.currentSlide) {
                    background = docs.slides[docs.currentSlide].backgroundAssetId || ""
                }
                backgroundCombo.currentIndex = 0
                for (var j = 0; j < docs.assets.length; ++j) {
                    if (docs.assets[j].id === background) {
                        backgroundCombo.currentIndex = j + 1
                        break
                    }
                }
            }
            Component.onCompleted: sync()
            Connections {
                target: docs
                function onDocumentsChanged() { stylePanel.sync() }
                function onNavigationChanged() { stylePanel.sync() }
            }
            Connections {
                target: editPage
                function onVisibleChanged() { if (editPage.visible) stylePanel.sync() }
            }

            Text {
                color: "#8f93a5"
                font.pixelSize: 10
                text: "STYLE"
            }

            ComboBox {
                id: styleCombo
                objectName: "styleCombo"
                width: parent.width
                textRole: "name"
                model: [{ id: "", name: "— show default —" }].concat(docs.styles)
                // Choosing a style overrides the current slide with it;
                // the first row (empty id) hands the slide back.
                onActivated: function (index) {
                    if (!docs.applyStyleToCurrentSlide(stylePanel.styleIdAt(index))) {
                        stylePanel.sync()
                    }
                }
            }

            Row {
                spacing: 6
                PanelButton {
                    objectName: "styleNewButton"
                    text: "New"
                    // Forks what the slide draws now for this slide only.
                    onClicked: docs.createStyleFromCurrent(
                                   fieldStyleName.text.trim() || "Style")
                }
                PanelButton {
                    objectName: "styleDefaultButton"
                    text: "Show default"
                    enabled: stylePanel.targetId() !== ""
                    onClicked: docs.setShowDefaultStyle(stylePanel.targetId())
                }
                PanelButton {
                    objectName: "styleFollowButton"
                    text: "Follow show"
                    enabled: docs.currentStyleIsOverride
                    onClicked: docs.applyStyleToCurrentSlide("")
                }
            }

            PanelField {
                id: fieldStyleName
                objectName: "styleNameField"
                width: parent.width
                onEditingFinished: stylePanel.commit()
            }

            Row {
                spacing: 8
                Column {
                    spacing: 2
                    Text {
                        color: "#8f93a5"
                        font.pixelSize: 10
                        text: "TEXT COLOUR"
                    }
                    PanelField {
                        id: fieldTextColor
                        objectName: "styleColourField"
                        width: 116
                        onEditingFinished: stylePanel.commit()
                    }
                }
                Column {
                    spacing: 2
                    Text {
                        color: "#8f93a5"
                        font.pixelSize: 10
                        text: "BACKGROUND"
                    }
                    PanelField {
                        id: fieldBackColour
                        objectName: "styleBackField"
                        width: 116
                        onEditingFinished: stylePanel.commit()
                    }
                }
            }

            Row {
                spacing: 8
                Column {
                    spacing: 2
                    Text {
                        color: "#8f93a5"
                        font.pixelSize: 10
                        text: "FONT"
                    }
                    PanelField {
                        id: fieldFontFamily
                        objectName: "styleFontField"
                        width: 156
                        onEditingFinished: stylePanel.commit()
                    }
                }
                Column {
                    spacing: 2
                    Text {
                        color: "#8f93a5"
                        font.pixelSize: 10
                        text: "SIZE"
                    }
                    PanelField {
                        id: fieldStyleSize
                        objectName: "styleSizeField"
                        width: 60
                        onEditingFinished: stylePanel.commit()
                    }
                }
            }

            Row {
                spacing: 8
                Row {
                    spacing: 4
                    Repeater {
                        model: [
                            { key: "left", label: "L" },
                            { key: "center", label: "C" },
                            { key: "right", label: "R" },
                        ]
                        delegate: PanelButton {
                            required property var modelData
                            objectName: "styleAlign" + modelData.key
                            text: modelData.label
                            highlighted: (docs.currentStyle.align || "center") === modelData.key
                            onClicked: stylePanel.commit(modelData.key)
                        }
                    }
                }
                Column {
                    spacing: 2
                    Text {
                        color: "#8f93a5"
                        font.pixelSize: 10
                        text: "LINE SPACING"
                    }
                    PanelField {
                        id: fieldLineSpacing
                        objectName: "styleSpacingField"
                        width: 60
                        onEditingFinished: stylePanel.commit()
                    }
                }
            }

            Text {
                color: "#8f93a5"
                font.pixelSize: 10
                text: "BACKGROUND IMAGE"
            }

            ComboBox {
                id: backgroundCombo
                objectName: "backgroundCombo"
                width: parent.width
                textRole: "name"
                model: [{ id: "", name: "None" }].concat(docs.assets)
                onActivated: function (index) {
                    var assetId = ""
                    if (index > 0 && index - 1 < docs.assets.length) {
                        assetId = docs.assets[index - 1].id
                    }
                    if (!docs.setSlideBackground(assetId)) {
                        stylePanel.sync()
                    }
                }
            }
        }
    }

    /* ------------------------------------------------------------------
     * The settings page (task 4.5): the essential settings, persisted
     * through AppSettings — presenter number keys and the F11
     * fullscreen — plus a cheat sheet of the shortcuts the keyboard
     * flows rely on, and the service information rows. The reference
     * keeps its shortcuts hardcoded and undocumented; documenting them
     * in-app is what makes "core flows work by keyboard" learnable.
     * ------------------------------------------------------------------ */
    Item {
        id: settingsPage
        visible: root.page === "settings"
        anchors.left: leftDock.right
        anchors.right: rightDock.left
        anchors.top: topBar.bottom
        anchors.bottom: libraryDrawer.top
        anchors.margins: 12

        Column {
            width: Math.min(620, parent.width)
            spacing: 5

            Text {
                color: "#f0f0ff"
                font.pixelSize: 15
                font.bold: true
                text: "Settings"
            }

            Text {
                color: "#8f93a5"
                font.pixelSize: 10
                topPadding: 10
                text: "PRESENTER"
            }
            CheckBox {
                text: "Number keys play slides (1\u20139, 0 = ten)"
                font.pixelSize: 13
                checked: appSettings.numberKeysPresent
                onToggled: appSettings.numberKeysPresent = checked
            }
            Text {
                color: "#8f93a5"
                font.pixelSize: 11
                wrapMode: Text.WordWrap
                width: parent.width
                text: "Presenter keys always work: Space, PageDown, PageUp, arrows, Home and End walk the current show straight onto the output."
            }

            Text {
                color: "#8f93a5"
                font.pixelSize: 10
                topPadding: 10
                text: "WINDOW"
            }
            CheckBox {
                text: "Fullscreen (F11)"
                font.pixelSize: 13
                checked: appSettings.fullscreen
                onToggled: appSettings.fullscreen = checked
            }

            Text {
                color: "#8f93a5"
                font.pixelSize: 10
                topPadding: 10
                text: "SHORTCUTS"
            }
            Repeater {
                model: [
                    { key: "Space / PageDown / Right / F5",
                      what: "Next slide \u2014 straight to the output" },
                    { key: "PageUp / Left", what: "Previous slide" },
                    { key: "Home / End", what: "First / last slide" },
                    { key: ". ", what: "Clear everything on air" },
                    { key: "Esc", what: "Clear all \u2014 or restore; deselects first on the edit page" },
                    { key: "F1 / F2", what: "Clear the background / the slide" },
                    { key: "Ctrl+1 / Ctrl+2 / Ctrl+5", what: "Show, edit and settings pages" },
                    { key: "Ctrl+F", what: "Library search" },
                    { key: "Ctrl+D", what: "Duplicate the slide \u2014 opens the drawer when there is none" },
                    { key: "Ctrl+C / Ctrl+X / Ctrl+V", what: "Copy / cut / paste the slide \u2014 text fields keep theirs" },
                    { key: "Ctrl+Z \u00b7 Ctrl+Y / Ctrl+Shift+Z", what: "Undo \u00b7 redo" },
                    { key: "Ctrl+S", what: "Save the service file" },
                    { key: "Arrows / Ctrl+Arrows", what: "Nudge the selected box 1px / 10px (edit page)" },
                    { key: "F11", what: "Fullscreen" }
                ]
                delegate: Row {
                    required property var modelData
                    spacing: 14
                    Text {
                        width: 250
                        color: "#f0f0ff"
                        font.pixelSize: 12
                        text: modelData.key
                    }
                    Text {
                        color: "#8f93a5"
                        font.pixelSize: 12
                        text: modelData.what
                    }
                }
            }

            Text {
                color: "#8f93a5"
                font.pixelSize: 10
                topPadding: 10
                text: "SERVICE"
            }
            Row {
                spacing: 14
                Text {
                    width: 250
                    color: "#f0f0ff"
                    font.pixelSize: 12
                    text: "Version"
                }
                Text {
                    color: "#8f93a5"
                    font.pixelSize: 12
                    text: applicationVersion + " \u00b7 worker " + worker.status
                }
            }
            Row {
                spacing: 14
                Text {
                    width: 250
                    color: "#f0f0ff"
                    font.pixelSize: 12
                    text: "Service directory"
                }
                Text {
                    color: "#8f93a5"
                    font.pixelSize: 12
                    text: docs.serviceDirectory
                }
            }
            Row {
                spacing: 14
                Text {
                    width: 250
                    color: "#f0f0ff"
                    font.pixelSize: 12
                    text: "Recovery autosave"
                }
                Text {
                    color: "#8f93a5"
                    font.pixelSize: 12
                    text: "saved with every edit \u00b7 Ctrl+S writes the service file"
                }
            }
        }
    }

    /* ------------------------------------------------------------------
     * Bottom library drawer (task 4.2): the searchable Shows/Media strip.
     * Its height is WorkspacePanels' (300 open, 40 closed). The type is
     * named LibraryDrawer so it doesn't shadow QtQuick.Controls' Drawer.
     * ------------------------------------------------------------------ */
    LibraryDrawer {
        id: libraryDrawer
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
    }
}
