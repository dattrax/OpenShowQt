import QtQuick
import QtQuick.Controls

/*
 * The left dock (task 6.19b5, from Main.qml's 4.2 panel): the show page of
 * the reference — the playlist chips on top (with the current one), the
 * running order itself below. Rows reorder by drag or the small handles
 * (drop targets compute the gap), rows arriving from the drawer insert at
 * the drop point, and everything goes through the undoable controller (§5).
 *
 * The split's row asked for "explicit signals for insert/remove/move/
 * select"; reading the real block, those commands were always the
 * controller's — every gesture calls `docs` (the context property) and the
 * rows it renders are its projection rows (playlists, playlistEntries with
 * each show resolved). There is no window state in here to signal about, so
 * like OutputPanel this component takes no properties and emits no signals:
 * the window's remaining ownership is where the dock sits and the id the
 * centre pages anchor to. What the gesture code has to get right is
 * positional arithmetic — the nearest-row drop and the after-the-move
 * toIndex — and that moves with the gestures, not out of them.
 */
SidePanel {
    id: playlistPanel
    side: "left"
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
