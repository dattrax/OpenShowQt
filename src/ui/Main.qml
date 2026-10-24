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

    Material.theme: Material.Dark
    Material.accent: "#f0008c"  // --secondary

    // Task 1.2: closing the UI shuts the worker down before the window
    // actually goes away.
    onClosing: function(close) {
        worker.shutdownNow()
    }

    // Task 3.3: once a slide has been presented from the panel, navigating
    // follows it onto the output — that is the operator's expectation
    // mid-service. Any Present click (re)arms it.
    property bool following: false

    function presentCurrentSlide() {
        const json = docs.currentSceneJson()
        if (json !== "") {
            worker.presentSceneJson(json)
        }
    }

    // Task 3.4: the reference's presenter keys. Escape clears all — and
    // restores when the output is already clear, matching the button's
    // Clear-all/Restore toggle. Disabled controls (nothing on air) simply
    // do nothing.
    Shortcut {
        sequence: StandardKey.Cancel
        enabled: worker.wordsOnAir || worker.backgroundOnAir || worker.canRestore
        onActivated: worker.canRestore ? worker.restoreOutput() : worker.clearAll()
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

    // Task 4.2: the drawer keys the reference carries (Ctrl+D toggles the
    // library strip, Ctrl+F opens its search, Ctrl+1..2 pick the tabs with
    // content; 3..8 stay with the disabled tabs).
    Shortcut {
        sequence: "Ctrl+D"
        onActivated: panels.toggleDrawer()
    }
    Shortcut {
        sequence: "Ctrl+F"
        onActivated: libraryDrawer.focusSearch()
    }
    Shortcut {
        sequence: "Ctrl+1"
        onActivated: libraryDrawer.openTab("shows")
    }
    Shortcut {
        sequence: "Ctrl+2"
        onActivated: libraryDrawer.openTab("media")
    }

    TopBar {
        id: topBar
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
     * Center — the Show page analogue: the current song's slide list and
     * the lyric editor. Everything here drives DocumentController
     * ("docs"); presenting compiles the selected slide and hands the JSON
     * to the worker, which presents on the acknowledgement.
     * ------------------------------------------------------------------ */
    Item {
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
                    contentItem: Text {
                        text: control.text
                        color: control.highlighted ? "#ffffff" : "#f0f0ff"
                        font.pixelSize: 13
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
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
