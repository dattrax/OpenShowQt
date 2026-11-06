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
    property alias selectedEntryId: selection.currentKey
    readonly property var selectedEntryIds: selection.keys
    readonly property var entryIds: docs.playlistEntries.map(row => row.entryId)
    ListSelection { id: selection }
    onEntryIdsChanged: if (selection) selection.retain(entryIds)
    function removeSelected() {
        if (docs.removePlaylistEntries(selection.keys.slice())) selection.clear()
    }
    function browseSelection() {
        const index = entryIds.indexOf(selection.currentKey)
        if (index < 0) return
        const showId = docs.playlistEntries[index].showId
        entryList.positionViewAtIndex(index, ListView.Contain)
        docs.selectSong(showId)
        servicePanel.entryChosen(selection.currentKey)
    }
    Keys.enabled: servicePanel.activeFocus
    Keys.onPressed: event => {
        if (event.key === Qt.Key_Delete) {
            servicePanel.removeSelected()
            event.accepted = true
        } else if (event.key === Qt.Key_Up || event.key === Qt.Key_Down) {
            selection.move(event.key === Qt.Key_Down ? 1 : -1, servicePanel.entryIds, event.modifiers)
            servicePanel.browseSelection()
            event.accepted = true
        } else if (event.key === Qt.Key_A && (event.modifiers & Qt.ControlModifier)) {
            selection.selectAll(servicePanel.entryIds)
            event.accepted = true
        }
    }
    property string dropTargetPlaylistId: ""

    signal entryChosen(string entryId)

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
            spacing: 4
            Text {
                id: serviceTitle
                anchors.verticalCenter: parent.verticalCenter
                text: "CURRENT SERVICE"
                color: "#b0b0b0"; font.pixelSize: 11; font.bold: true
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                width: Math.max(0, parent.width - serviceTitle.width - 4)
                horizontalAlignment: Text.AlignRight
                elide: Text.ElideRight
                text: servicePanel.playlistName() + " · " + docs.playlistEntries.length + " cues"
                color: "#b0b0b0"; font.pixelSize: 11
            }
        }

        Rectangle {
            id: entryBox
            width: parent.width
            height: Math.max(80, parent.height - 30)
            color: "#242424"
            border.color: entryDrop.containsDrag ? "#1685f8" : "#4a4a4a"
            border.width: entryDrop.containsDrag ? 3 : 1

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
                    opacity: entryMouse.drag.active ? 0.45 : 1
                    readonly property string rowEntryId: modelData.entryId
                    readonly property bool rowSelected:
                        selection.contains(modelData.entryId)
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
                                         (servicePanel.editingEnabled ? 86 : 46))
                        elide: Text.ElideRight
                        font.pixelSize: 13
                        color: entryRow.rowSelected ? "#ffffff" : "#eeeeee"
                        text: (entryRow.modelData.section !== ""
                               ? entryRow.modelData.section + " \u00b7 " : "")
                              + entryRow.modelData.showName
                    }
                    Text {
                        anchors.right: parent.right
                        anchors.rightMargin: servicePanel.editingEnabled ? 56 : 8
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
                                 step: -1},
                                {glyph: "\u25bc",
                                 enabled: entryRow.index < entryList.count - 1,
                                 step: 1}
                            ]
                            delegate: Text {
                                id: handle
                                required property var modelData
                                width: 20
                                height: 24
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                                font.pixelSize: 9
                                color: !modelData.enabled ? "#606060"
                                     : (handleMouse.containsMouse ? "#eeeeee" : "#b0b0b0")
                                text: modelData.glyph
                                MouseArea {
                                    id: handleMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    enabled: handle.modelData.enabled
                                    onClicked: {
                                        docs.movePlaylistEntry(
                                            entryRow.modelData.entryId,
                                            entryRow.index + handle.modelData.step)
                                    }
                                }
                            }
                        }
                    }

                    MouseArea {
                        id: entryMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        preventStealing: true
                        cursorShape: drag.active ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                        drag.smoothed: false
                        drag.target: entryDragProxy
                        onPressed: function(mouse) {
                            servicePanel.forceActiveFocus()
                            entryList.currentIndex = entryRow.index
                            entryDragProxy.begin(entryRow, mouse.x, mouse.y)
                        }
                        onClicked: function(mouse) {
                            selection.select(entryRow.modelData.entryId, servicePanel.entryIds, mouse.modifiers)
                            docs.selectSong(entryRow.modelData.showId)
                            servicePanel.entryChosen(entryRow.modelData.entryId)
                        }
                        onReleased: {
                            if (entryDragProxy.Drag.active) entryDragProxy.Drag.drop()
                            entryDragProxy.x = 0
                            entryDragProxy.y = 0
                        }
                        onCanceled: { entryDragProxy.x = 0; entryDragProxy.y = 0 }
                    }

                    DragFeedback {
                        id: entryDragProxy
                        payload: "entry:" + entryRow.modelData.entryId
                        title: entryRow.modelData.showName
                        hint: "Move within the service"
                        moving: true
                        dragging: entryMouse.drag.active
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
                objectName: "serviceDropArea"
                anchors.fill: parent
                keys: ["text/plain"]
                property string payloadKind: ""
                readonly property int dropAction: payloadKind === "entry" ? Qt.MoveAction : Qt.CopyAction
                readonly property string dropHint: payloadKind === "entry" ? "Move to the marked position"
                                                                         : "Add at the marked position"
                readonly property int insertionIndex: servicePanel.gapIndex(dragY)
                // Bounded edge autoscroll (SL.7): a drag held near the top or
                // bottom of a long order scrolls toward the destination it is
                // hovering, one small step per tick, and stops on exit/drop.
                property real dragY: 0
                onEntered: drag => {
                    const payload = drag.source ? String(drag.source.payload || "") : ""
                    const colon = payload.indexOf(":")
                    const kind = payload.substring(0, colon)
                    const id = payload.substring(colon + 1)
                    const valid = id && (kind === "entry" ? servicePanel.entryIds.indexOf(id) >= 0
                        : (kind === "show" ? !!docs.showInfo(id).id
                           : (kind === "catalog" && !!library.itemOf(id).id)))
                    drag.accepted = !!valid
                    if (!valid) return
                    entryDrop.payloadKind = kind
                    drag.accept(kind === "entry" ? Qt.MoveAction : Qt.CopyAction)
                    entryDrop.dragY = drag.y
                    autoScroll.start()
                }
                onPositionChanged: drag => { entryDrop.dragY = drag.y }
                onExited: autoScroll.stop()
                Timer {
                    id: autoScroll
                    interval: 40
                    repeat: true
                    onTriggered: {
                        if (entryDrop.dragY < 36)
                            entryList.contentY = Math.max(0, entryList.contentY - 20)
                        else if (entryDrop.dragY > entryList.height - 36)
                            entryList.contentY = Math.min(
                                Math.max(0, entryList.contentHeight - entryList.height),
                                entryList.contentY + 20)
                    }
                }
                onDropped: function (drop) {
                    autoScroll.stop()
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
                    if (kind === "catalog" || kind === "show" || kind === "entry")
                        servicePanel.forceActiveFocus()
                    if (kind === "catalog") {
                        // COPY: the Library keeps the item; the service gains
                        // a linked show (or another cue over the existing one).
                        if (songService.addToService(id, at)) {
                            drop.accept(Qt.CopyAction)
                            servicePanel.entryChosen("")
                        }
                    } else if (kind === "show") {
                        if (docs.insertShowInPlaylist(id, at)) {
                            drop.accept(Qt.CopyAction)
                            servicePanel.entryChosen("")
                        }
                    } else if (kind === "entry") {
                        let from = -1
                        const entries = docs.playlistEntries
                        for (let i = 0; i < entries.length; ++i) {
                            if (entries[i].entryId === id) {
                                from = i
                                break
                            }
                        }
                        const to = from < at ? at - 1 : at
                        if (from >= 0 && (from === to || docs.movePlaylistEntry(id, to))) {
                            drop.accept(Qt.MoveAction)
                            selection.select(id, servicePanel.entryIds, Qt.NoModifier)
                            servicePanel.entryChosen(id)
                        }
                    }
                }
            }

            Rectangle {
                objectName: "serviceDropIndicator"
                visible: entryDrop.containsDrag
                z: 2
                x: 3
                y: Math.max(2, Math.min(entryBox.height - 6,
                    entryList.y + entryDrop.insertionIndex * 30 - entryList.contentY - 2))
                width: entryBox.width - 6
                height: 4
                radius: 2
                color: "#65c4ff"
            }
        }
    }
}
