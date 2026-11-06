import QtQuick
import QtQuick.Controls

/*
 * UI part: CurrentServicePanel (tasks SL.4 and SL.7).
 *
 * The ONE running order under preparation, below the shared Library. There is
 * no service picker in this milestone (operator amendment 2026-11-06): Save
 * and Load already move a service, so this pane names the current one and
 * edits its cues.
 *
 * Drops: catalog:<id> adds a reference-backed song show; show:<id> adds an
 * authored presentation; entry:<id> reorders a cue. The insertion gap is
 * computed from the scrolled list's own row geometry, and cancel/stale ids
 * change nothing.
 */
Item {
    id: servicePanel
    objectName: "currentServicePanel"

    property bool editingEnabled: false
    property string selectedEntryId: ""
    property string dropTargetPlaylistId: ""

    signal entryChosen(string showId)

    function playlistName() {
        const rows = docs.playlists
        for (let i = 0; i < rows.length; ++i)
            if (rows[i].id === docs.currentPlaylistId)
                return rows[i].name
        return "?"
    }
    function gapIndex(y) {
        // The list's content is a flat 30px row run; add the scrolled offset
        // so a drop on a scrolled order lands beside the row under it.
        const raw = (y - entryList.y + entryList.contentY) / 30.0
        return Math.max(0, Math.min(entryList.count, Math.round(raw)))
    }

    Column {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 4

        Row {
            width: parent.width
            height: 24
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "CURRENT SERVICE"
                color: "#b0b0b0"; font.pixelSize: 11; font.bold: true
            }
            Item { width: Math.max(0, parent.width - 180); height: 1 }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                width: 170
                horizontalAlignment: Text.AlignRight
                elide: Text.ElideRight
                text: servicePanel.playlistName() + " \u00b7 "
                      + docs.playlistEntries.length + " cues"
                color: "#b0b0b0"; font.pixelSize: 11
            }
        }

        Rectangle {
            id: entryBox
            width: parent.width
            height: Math.max(80, parent.height - 30)
            color: "#242424"
            border.color: entryDrop.containsDrag ? "#1685f8" : "#4a4a4a"

            ListView {
                id: entryList
                objectName: "collectionPlaylistList"
                anchors.fill: parent
                anchors.margins: 1
                clip: true
                model: docs.playlistEntries
                ScrollBar.vertical: ScrollBar {}

                delegate: Item {
                    id: entryRow
                    required property var modelData
                    required property int index
                    objectName: "collectionPlaylistRow"
                    width: entryList.width
                    height: 30
                    readonly property string rowEntryId: modelData.entryId
                    readonly property bool rowSelected:
                        modelData.entryId === servicePanel.selectedEntryId
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
                                         (servicePanel.editingEnabled ? 106 : 46))
                        elide: Text.ElideRight
                        font.pixelSize: 13
                        color: entryRow.rowSelected ? "#ffffff" : "#eeeeee"
                        text: (entryRow.modelData.section !== ""
                               ? entryRow.modelData.section + " \u00b7 " : "")
                              + entryRow.modelData.showName
                    }
                    Text {
                        anchors.right: parent.right
                        anchors.rightMargin: servicePanel.editingEnabled ? 76 : 8
                        anchors.verticalCenter: parent.verticalCenter
                        font.pixelSize: 11
                        color: entryRow.rowSelected ? "#d9ebff" : "#b0b0b0"
                        text: entryRow.modelData.slideCount
                    }

                    Row {
                        z: 1
                        visible: servicePanel.editingEnabled
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
                                            docs.removePlaylistEntry(
                                                entryRow.modelData.entryId)
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
                        drag.target: servicePanel.editingEnabled ? entryDragProxy : null
                        onClicked: {
                            servicePanel.selectedEntryId = entryRow.modelData.entryId
                            docs.selectSong(entryRow.modelData.showId)
                            servicePanel.entryChosen(entryRow.modelData.showId)
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
                        Drag.mimeData: ({"text/plain": entryDragProxy.payload})
                        Drag.hotSpot: Qt.point(width / 2, height / 2)
                    }
                }

                Text {
                    anchors.centerIn: parent
                    visible: entryList.count === 0
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    color: "#929292"; font.pixelSize: 13
                    text: "This running order is empty \u2014 drag a song or presentation here"
                }
            }

            DropArea {
                id: entryDrop
                anchors.fill: parent
                keys: ["text/plain"]
                onDropped: function (drop) {
                    const payload = drop.source && drop.source.payload
                        ? drop.source.payload : drop.getDataAsString("text/plain")
                    const colon = payload.indexOf(":")
                    if (colon <= 0)
                        return
                    const kind = payload.substring(0, colon)
                    const id = payload.substring(colon + 1)
                    if (!id)
                        return
                    const at = servicePanel.gapIndex(drop.y)
                    if (kind === "catalog") {
                        // COPY: the Library keeps the item; the service gains
                        // a linked show (or another cue over the existing one).
                        drop.accept()
                        songService.addToService(id, at)
                    } else if (kind === "show") {
                        drop.accept()
                        docs.insertShowInPlaylist(id, at)
                    } else if (kind === "entry") {
                        let from = -1
                        const entries = docs.playlistEntries
                        for (let i = 0; i < entries.length; ++i) {
                            if (entries[i].entryId === id) {
                                from = i
                                break
                            }
                        }
                        drop.accept()
                        docs.movePlaylistEntry(id, from >= 0 && from < at ? at - 1 : at)
                    }
                }
            }
        }
    }
}
