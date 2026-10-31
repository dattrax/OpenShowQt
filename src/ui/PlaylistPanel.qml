import QtQuick
import QtQuick.Controls

// Library above the running order, as in the supplied operator reference.
SidePanel {
    id: playlistPanel
    side: "left"
    property bool editingEnabled: false
    width: panels.leftWidth
    onDraggedTo: candidate => panels.resizeLeft(candidate)
    onStripClicked: panels.clickLeft()

    Item {
        anchors.fill: parent
        anchors.margins: 10

        Text {
            id: libraryTitle
            text: "LIBRARY"
            color: "#b0b0b0"; font.pixelSize: 11; font.bold: true
        }
        ListView {
            id: libraryBox
            anchors.top: libraryTitle.bottom; anchors.topMargin: 8
            width: parent.width
            height: Math.min(150, Math.max(65, parent.height * 0.25))
            clip: true
            model: docs.songs
            ScrollBar.vertical: ScrollBar {}
            delegate: ItemDelegate {
                required property var modelData
                width: libraryBox.width; height: 28
                text: modelData.name
                font.pixelSize: 12
                highlighted: modelData.id === docs.currentSongId
                onClicked: docs.selectSong(modelData.id)
                onDoubleClicked: docs.addShowToPlaylist(modelData.id)
                background: Rectangle {
                    color: parent.highlighted ? "#1678cf" : parent.hovered ? "#4a4a4a" : "transparent"
                }
            }
        }

        Text {
            id: playlistsTitle
            anchors.top: libraryBox.bottom
            anchors.topMargin: 14
            anchors.left: parent.left
            color: "#b0b0b0"
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
                width: Math.max(0, playlistRow.width - (newPlaylistButton.visible ? newPlaylistButton.width + 6 : 0))
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
                    color: playlistChip.modelData.id === docs.currentPlaylistId ? "#1685f8" : "#242424"
                    border.color: "#4a4a4a"

                    Text {
                        id: chipText
                        anchors.centerIn: parent
                        font.pixelSize: 12
                        color: playlistChip.modelData.id === docs.currentPlaylistId ? "#ffffff" : "#eeeeee"
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
                visible: playlistPanel.editingEnabled

                width: 32
                height: 26
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

        Text {
            id: playlistTitle
            anchors.top: playlistRow.bottom
            anchors.topMargin: 10
            anchors.left: parent.left
            color: "#b0b0b0"
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
            color: "#242424"
            border.color: "#4a4a4a"

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
                        color: entryRow.modelData.showId === docs.currentSongId ? "#1685f8"
                             : (entryMouse.containsMouse ? "#4a4a4a" : "transparent")
                    }

                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 6
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.max(0, entryRow.width - (playlistPanel.editingEnabled ? 76 : 12))
                        elide: Text.ElideRight
                        font.pixelSize: 13
                        color: entryRow.modelData.showId === docs.currentSongId ? "#ffffff" : "#eeeeee"
                        text: (entryRow.modelData.section !== ""
                               ? entryRow.modelData.section + " \u00b7 " : "")
                              + entryRow.modelData.showName
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
                        onClicked: docs.selectSong(entryRow.modelData.showId)
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
                    visible: entryList.count === 0
                    color: "#929292"
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
            color: docs.dirty ? "#d8c07a" : "#b0b0b0"
            font.pixelSize: 11
            text: (docs.dirty ? "\u25cf unsaved  \u00b7  " : "")
                  + (docs.status !== "" ? docs.status + "  \u00b7  " : "")
                  + docs.serviceDirectory
        }
    }
}
