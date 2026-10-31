import QtQuick
import QtQuick.Controls

// UI.2: ONE collection outline (the Library plus every playlist) above ONE
// detail view of the selected collection's contents — the flat library and
// the horizontal playlist chips this panel used to stack are gone.
// Selecting an outline item only decides what the detail list shows; the
// audience sees nothing until a cue is triggered. Every row here is a
// read-only projection (docs.collectionOutline, docs.songs,
// docs.playlistEntries); preparation actions — adding a show to the running
// order, dragging rows — stay separate from live slide triggers, and the
// playlist's entry ids give repeated entries of the same show one stable
// identity each.
SidePanel {
    id: playlistPanel
    side: "left"
    property bool editingEnabled: false
    width: panels.leftWidth
    onDraggedTo: candidate => panels.resizeLeft(candidate)
    onStripClicked: panels.clickLeft()

    // Which outline item's contents the detail view is showing. Selecting a
    // playlist also makes it current (that is what add/drop prepare against);
    // selecting the Library changes only this view, never the running order
    // and never the output.
    property string outlineKind: "playlist"
    property string outlinePlaylistId: docs.currentPlaylistId
    // The entry row the operator last clicked. Keyed by ENTRY id, so a show
    // that appears twice in the order shows one selection, not two.
    property string selectedEntryId: ""

    function selectOutline(kind, id) {
        if (kind === "playlist" && id !== "") {
            docs.selectPlaylist(id)
            playlistPanel.outlineKind = "playlist"
            playlistPanel.outlinePlaylistId = id
        } else {
            playlistPanel.outlineKind = "library"
        }
        playlistPanel.selectedEntryId = ""
    }

    // A library row dropped on a playlist outline row: the target becomes the
    // order being prepared and takes the show at its end. This runs at the
    // panel's own scope because inserting the show rebinds the outline model
    // and would destroy the delegate whose handler called it — everything the
    // drop needs is read before the first mutation.
    function receiveShowDrop(playlistId, showId, entryCount) {
        docs.selectPlaylist(playlistId)
        playlistPanel.outlineKind = "playlist"
        playlistPanel.outlinePlaylistId = playlistId
        playlistPanel.selectedEntryId = ""
        docs.insertShowInPlaylist(showId, entryCount)
    }

    // True while a drop of a library row is hovering a playlist outline row.
    property string dropTargetPlaylistId: ""

    // The viewed order IS the current playlist whenever a playlist stands
    // selected: creation, removal, undo and selectPlaylist all move the
    // controller's anchor, and the panel follows it rather than keeping a
    // stale id (the detail list binds docs.playlistEntries, which is the
    // CURRENT playlist's rows).
    Connections {
        target: docs
        function onNavigationChanged() {
            if (playlistPanel.outlineKind === "playlist"
                && playlistPanel.outlinePlaylistId !== docs.currentPlaylistId) {
                playlistPanel.outlinePlaylistId = docs.currentPlaylistId
                playlistPanel.selectedEntryId = ""
            }
        }
    }

    Item {
        anchors.fill: parent
        anchors.margins: 10

        // -----------------------------------------------------------------
        // The outline: Library + playlists, one row each, with counts.
        // -----------------------------------------------------------------
        Item {
            id: outlineHeader
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: 24
            Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                text: "COLLECTIONS"
                color: "#b0b0b0"; font.pixelSize: 11; font.bold: true
            }
            // Chip-styled (the Controls Button rendered its "+" pill
            // without the glyph at this size).
            Rectangle {
                id: newPlaylistButton
                visible: playlistPanel.editingEnabled
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                width: 32; height: 24
                radius: 4
                color: newPlaylistMouse.containsMouse ? "#4a4a4a" : "#242424"
                border.color: "#4a4a4a"
                Text {
                    anchors.centerIn: parent
                    font.pixelSize: 15
                    color: "#eeeeee"
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

        Rectangle {
            id: outlineBox
            anchors.top: outlineHeader.bottom; anchors.topMargin: 6
            anchors.left: parent.left
            anchors.right: parent.right
            height: Math.min(170, Math.max(96, parent.height * 0.3))
            color: "#242424"
            border.color: playlistPanel.dropTargetPlaylistId !== "" ? "#1685f8" : "#4a4a4a"

            ListView {
                id: outlineList
                objectName: "collectionOutline"
                anchors.fill: parent
                anchors.margins: 1
                clip: true
                model: docs.collectionOutline
                ScrollBar.vertical: ScrollBar {}

                delegate: Item {
                    id: outlineRow

                    required property var modelData
                    objectName: "collectionOutlineRow"
                    width: outlineList.width
                    height: 28
                    readonly property string outlineId: modelData.id
                    readonly property bool rowSelected:
                        modelData.kind === playlistPanel.outlineKind
                        && (modelData.kind === "library"
                            || modelData.id === playlistPanel.outlinePlaylistId)
                    Accessible.name: (modelData.kind === "library" ? "Collection: " : "Playlist: ")
                                     + modelData.name + ", " + modelData.count
                                     + (modelData.kind === "library" ? " presentations" : " cues")

                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: 1
                        color: dropArea.containsDrag ? "#1c4c7c"
                             : (outlineRow.rowSelected ? "#1685f8"
                                : (outlineMouse.containsMouse ? "#4a4a4a" : "transparent"))
                    }
                    Text {
                        id: outlineLabel
                        anchors.left: parent.left
                        anchors.leftMargin: modelData.kind === "library" ? 8 : 22
                        anchors.verticalCenter: parent.verticalCenter
                        width: outlineRow.width - (kindCount.width + 34)
                        elide: Text.ElideRight  // long names stay one row
                        font.pixelSize: 13
                        font.italic: modelData.kind === "library"
                        color: outlineRow.rowSelected ? "#ffffff" : "#eeeeee"
                        text: modelData.name
                    }
                    Text {
                        id: kindCount
                        anchors.right: parent.right
                        anchors.rightMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        font.pixelSize: 11
                        color: outlineRow.rowSelected ? "#d9ebff" : "#b0b0b0"
                        text: modelData.count
                    }
                    MouseArea {
                        id: outlineMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        acceptedButtons: Qt.LeftButton
                        onClicked: playlistPanel.selectOutline(outlineRow.modelData.kind,
                                                               outlineRow.modelData.id)
                    }
                    // A library row dragged onto a playlist prepares that
                    // playlist — it becomes the one being prepared and takes
                    // the show at its end. Browsing here presents nothing.
                    DropArea {
                        id: dropArea
                        anchors.fill: parent
                        keys: ["text/plain"]
                        enabled: outlineRow.modelData.kind === "playlist"
                        onEntered: drag => {
                            const dragged =
                                drag.source ? String(drag.source.payload || "") : ""
                            if (dragged.startsWith("show:")) {
                                playlistPanel.dropTargetPlaylistId = outlineRow.modelData.id
                                drag.accept(Qt.CopyAction)
                            }
                        }
                        onExited: playlistPanel.dropTargetPlaylistId = ""
                        onDropped: function (drop) {
                            playlistPanel.dropTargetPlaylistId = ""
                            const payload = drop.source && drop.source.payload
                                ? drop.source.payload : drop.getDataAsString("text/plain")
                            if (!payload.startsWith("show:")) {
                                return
                            }
                            const targetId = outlineRow.modelData.id
                            const targetCount = outlineRow.modelData.count
                            drop.accept()
                            playlistPanel.receiveShowDrop(targetId, payload.substring(5),
                                                          targetCount)
                        }
                    }
                }
            }
        }

        // -----------------------------------------------------------------
        // The detail view of whichever outline item stands selected.
        // -----------------------------------------------------------------
        Text {
            id: detailTitle
            anchors.top: outlineBox.bottom
            anchors.topMargin: 14
            anchors.left: parent.left
            anchors.right: parent.right
            color: "#b0b0b0"
            font.pixelSize: 11
            font.bold: true
            elide: Text.ElideRight
            text: (playlistPanel.outlineKind === "library"
                   ? "LIBRARY \u00b7 " + docs.songs.length + " presentations"
                   : "RUNNING ORDER \u00b7 "
                     + (docs.playlists.filter(p => p.id === playlistPanel.outlinePlaylistId)
                            .map(p => p.name).join("") || "?"))
        }

        Rectangle {
            id: detailBox
            anchors.top: detailTitle.bottom
            anchors.topMargin: 6
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: serviceDirText.top
            anchors.bottomMargin: 8
            color: "#242424"
            border.color: "#4a4a4a"

            // --- the Library's contents: one row per presentation ---
            ListView {
                id: libraryDetail
                objectName: "collectionLibraryList"
                anchors.fill: parent
                anchors.margins: 1
                clip: true
                visible: playlistPanel.outlineKind === "library"
                model: docs.songs
                ScrollBar.vertical: ScrollBar {}

                delegate: Item {
                    id: libraryRow

                    required property var modelData
                    objectName: "collectionLibraryRow"
                    width: libraryDetail.width
                    height: 28
                    readonly property string rowShowId: modelData.id
                    readonly property bool rowSelected: modelData.id === docs.currentSongId
                    Accessible.name: "Presentation: " + modelData.name + ", "
                                     + modelData.slideCount
                                     + (modelData.slideCount === 1 ? " slide" : " slides")

                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: 1
                        color: libraryRow.rowSelected ? "#1685f8"
                             : (rowMouse.containsMouse ? "#4a4a4a" : "transparent")
                    }
                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        width: libraryRow.width - 80
                        elide: Text.ElideRight
                        font.pixelSize: 13
                        color: libraryRow.rowSelected ? "#ffffff" : "#eeeeee"
                        text: modelData.name
                    }
                    Text {
                        anchors.right: parent.right
                        anchors.rightMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        font.pixelSize: 11
                        color: libraryRow.rowSelected ? "#d9ebff" : "#b0b0b0"
                        text: modelData.slideCount
                              + (modelData.slideCount === 1 ? " slide" : " slides")
                    }
                    MouseArea {
                        id: rowMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        drag.target: libraryDragProxy
                        acceptedButtons: Qt.LeftButton
                        // Selection and preparation only: choosing a row here
                        // changes what is inspected; double-click prepares it
                        // into the running order. Nothing reaches the worker.
                        onClicked: docs.selectSong(libraryRow.modelData.id)
                        onDoubleClicked: docs.addShowToPlaylist(libraryRow.modelData.id)
                        onReleased: {
                            if (libraryDragProxy.Drag.active) libraryDragProxy.Drag.drop()
                            libraryDragProxy.x = 0
                            libraryDragProxy.y = 0
                        }
                        onCanceled: { libraryDragProxy.x = 0; libraryDragProxy.y = 0 }
                    }
                    // The row drags out to a playlist outline row as
                    // text/plain "show:<id>" — the drop side reads it back.
                    Item {
                        id: libraryDragProxy
                        readonly property string payload: "show:" + libraryRow.modelData.id
                        Drag.source: libraryDragProxy
                        width: libraryRow.width; height: libraryRow.height
                        Drag.active: rowMouse.drag.active
                        Drag.keys: ["text/plain"]
                        Drag.mimeData: ({"text/plain": "show:" + libraryRow.modelData.id})
                        Drag.hotSpot: Qt.point(width / 2, height / 2)
                    }
                }

                Text {
                    anchors.centerIn: parent
                    visible: libraryDetail.count === 0
                    color: "#929292"
                    font.pixelSize: 13
                    horizontalAlignment: Text.AlignHCenter
                    width: parent.width
                    text: playlistPanel.editingEnabled
                          ? "No presentations \u2014 create one above"
                          : "No presentations \u2014 open Edit to create one"
                }
            }

            // --- a playlist's contents: the running order's cues ---
            ListView {
                id: playlistDetail
                objectName: "collectionPlaylistList"
                anchors.fill: parent
                anchors.margins: 1
                clip: true
                visible: playlistPanel.outlineKind === "playlist"
                model: docs.playlistEntries
                ScrollBar.vertical: ScrollBar {}

                delegate: Item {
                    id: entryRow

                    required property var modelData
                    required property int index
                    objectName: "collectionPlaylistRow"
                    width: playlistDetail.width
                    height: 30
                    // Stable identity: repeated entries of one show are two
                    // cues with two entry ids, and selection follows the id.
                    readonly property string rowEntryId: modelData.entryId
                    readonly property bool rowSelected:
                        modelData.entryId === playlistPanel.selectedEntryId
                    Accessible.name: "Cue " + (index + 1) + ": "
                                     + (modelData.section !== "" ? modelData.section + ", " : "")
                                     + modelData.showName

                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: 1
                        color: entryRow.rowSelected ? "#1685f8"
                             : (entryMouse.containsMouse ? "#4a4a4a" : "transparent")
                    }
                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 6
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.max(0, entryRow.width -
                                         (playlistPanel.editingEnabled ? 106 : 46))
                        elide: Text.ElideRight
                        font.pixelSize: 13
                        color: entryRow.rowSelected ? "#ffffff" : "#eeeeee"
                        text: (entryRow.modelData.section !== ""
                               ? entryRow.modelData.section + " \u00b7 " : "")
                              + entryRow.modelData.showName
                    }
                    Text {
                        anchors.right: parent.right
                        anchors.rightMargin: playlistPanel.editingEnabled ? 76 : 8
                        anchors.verticalCenter: parent.verticalCenter
                        font.pixelSize: 11
                        color: entryRow.rowSelected ? "#d9ebff" : "#b0b0b0"
                        text: entryRow.modelData.slideCount
                    }

                    // The reorder handles: the drag is the reference's
                    // path, these make the same move without a mouse.
                    Row {
                        z: 1
                        visible: playlistPanel.editingEnabled
                        anchors.right: parent.right
                        anchors.rightMargin: 2
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 2
                        Repeater {
                            model: [
                                {glyph: "\u25b2", enabled: entryRow.index > 0,
                                 step: -1, action: "move"},
                                {glyph: "\u25bc",
                                 enabled: entryRow.index < playlistDetail.count - 1,
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
                                color: !modelData.enabled ? "#606060"
                                     : (handleMouse.containsMouse ? "#eeeeee" : "#b0b0b0")
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
                        drag.target: playlistPanel.editingEnabled ? entryDragProxy : null
                        // Clicking a cue selects it and its presentation for
                        // inspection — it does not go to the output. Live
                        // triggers are the slide workspace's (UI.3b).
                        onClicked: {
                            playlistPanel.selectedEntryId = entryRow.modelData.entryId
                            docs.selectSong(entryRow.modelData.showId)
                        }
                        onReleased: {
                            if (entryDragProxy.Drag.active) entryDragProxy.Drag.drop()
                            entryDragProxy.x = 0
                            entryDragProxy.y = 0
                        }
                        onCanceled: { entryDragProxy.x = 0; entryDragProxy.y = 0 }
                    }

                    Item {
                        id: entryDragProxy
                        readonly property string payload: "entry:" + entryRow.modelData.entryId
                        Drag.source: entryDragProxy
                        width: entryRow.width; height: entryRow.height
                        Drag.active: entryMouse.drag.active
                        Drag.keys: ["text/plain"]
                        Drag.mimeData: ({"text/plain": "entry:" + entryRow.modelData.entryId})
                        Drag.hotSpot: Qt.point(width / 2, height / 2)
                    }
                }

                Text {
                    anchors.centerIn: parent
                    visible: playlistDetail.count === 0
                    color: "#929292"
                    font.pixelSize: 13
                    horizontalAlignment: Text.AlignHCenter
                    width: parent.width
                    text: "This running order is empty"
                }
            }

            // The detail's drop target: drops land in the gap nearest the
            // pointer (rows are 30 high), shows insert and entries move
            // around the row being dragged. A show row dropped here joins
            // the running order being viewed — preparation, not air.
            DropArea {
                anchors.fill: parent
                enabled: playlistPanel.outlineKind === "playlist"
                keys: ["text/plain"]

                onDropped: function (drop) {
                    const payload = drop.source && drop.source.payload
                        ? drop.source.payload : drop.getDataAsString("text/plain")
                    const parts = payload.split(":")
                    const kind = parts[0]
                    const id = parts[1]
                    if (!id) {
                        return
                    }
                    // Round to the nearest row boundary: above the
                    // middle of row b means index b, below means b+1.
                    const index = Math.round((drop.y - playlistDetail.y) / 30)
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
            color: docs.dirty ? "#d8c07a" : "#b0b0b0"
            font.pixelSize: 11
            text: docs.dirty ? "\u25cf Unsaved changes" : "No unsaved changes"
        }
    }
}
