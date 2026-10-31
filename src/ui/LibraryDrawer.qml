import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs

// The library below the running order and slide grid. Show allows finding
// presentations and arranging the playlist; content authoring is Edit-only.
FocusScope {
    id: drawer

    Rectangle { anchors.fill: parent; color: "#353535"; z: -1 }
    height: panels.drawerHeight
    property bool editingEnabled: false
    clip: true

    readonly property bool open: height > 40.5
    property string tab: "shows"
    property string query: ""
    property bool searchActive: false
    property string selectedAssetId: ""

    // True while any part of the drawer holds the keyboard — Main's
    // Ctrl+1/2 page keys step aside exactly while it does.
    readonly property bool containsFocus: drawer.activeFocus

    function openTab(id) {
        tab = id
        if (!open) {
            panels.toggleDrawer()
        }
    }

    function focusSearch() {
        searchActive = true
        Qt.callLater(() => searchField.forceActiveFocus())
    }

    // Ctrl+1/2 belong to the pages from the window (Main, task 4.3);
    // from inside the drawer — where the search caret is — they still
    // pick the Shows and Media tabs. The two owners gate on this flag,
    // so exactly one of each pair is ever live (Qt treats two enabled
    // shortcuts with the same key as ambiguous and drops both).
    Shortcut {
        sequence: "Ctrl+1"
        enabled: drawer.containsFocus
        onActivated: drawer.openTab("shows")
    }
    Shortcut {
        sequence: "Ctrl+2"
        enabled: drawer.containsFocus
        onActivated: drawer.openTab("media")
    }

    // Enter with a query: the reference's placement — land the first
    // match after the current show and open it.
    function searchEnter() {
        if (tab !== "shows") {
            return
        }
        const hits = docs.searchShows(query)
        if (hits.length === 0) {
            return
        }
        const id = hits[0].id
        docs.insertShowAfterCurrentShow(id)
        docs.selectSong(id)
        query = ""
    }

    // ---------------------------------------------------------------------
    // The 40px bar: tabs left, search right. Visible open or closed.
    // ---------------------------------------------------------------------
    Item {
        id: tabStrip
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 40

        // The bar itself is the toggle (the reference's top-strip click).
        MouseArea {
            anchors.fill: parent
            onClicked: panels.toggleDrawer()
        }

        Row {
            anchors.left: parent.left
            anchors.leftMargin: 6

            Repeater {
                model: [
                    {id: "shows", label: "Shows", enabled: true},
                    {id: "media", label: "Media", enabled: true}
                ]

                delegate: Item {
                    id: tabButton

                    required property var modelData
                    readonly property bool active: drawer.tab === modelData.id
                    width: tabLabel.width + 24
                    height: 40

                    Text {
                        id: tabLabel
                        anchors.centerIn: parent
                        font.pixelSize: 12
                        color: tabButton.active ? "#eeeeee"
                             : (tabButton.modelData.enabled ? "#b0b0b0" : "#777777")
                        text: tabButton.modelData.label
                    }

                    Rectangle {
                        width: parent.width
                        height: 2
                        anchors.bottom: parent.bottom
                        visible: tabButton.active
                        color: "#1685f8"  // --secondary
                    }

                    // The reference toggles the active tab (click it again
                    // to shut the drawer) and opens on any other enabled
                    // tab. Ctrl+1..8 mirror the same switch.
                    MouseArea {
                        anchors.fill: parent
                        enabled: tabButton.modelData.enabled
                        onClicked: {
                            if (tabButton.active && drawer.open) {
                                panels.toggleDrawer()
                            } else {
                                drawer.openTab(tabButton.modelData.id)
                            }
                        }
                    }
                }
            }
        }

        // Search: the 100px button swaps for the 290px input (reference
        // §4, verified live), and the swap survives the drawer being
        // closed because the bar is what shows then.
        Item {
            anchors.right: parent.right
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            width: drawer.searchActive ? 290 : searchButton.width
            height: 36

            PanelButton {
                id: searchButton
                visible: !drawer.searchActive
                width: 100
                height: 36
                text: "Search"
                onClicked: drawer.focusSearch()
            }

            TextField {
                id: searchField
                objectName: "librarySearchField"
                visible: drawer.searchActive
                width: 290
                height: 36
                placeholderText: "Search..."
                placeholderTextColor: "#929292"
                color: "#eeeeee"
                text: drawer.query
                onTextChanged: drawer.query = text
                onAccepted: drawer.searchEnter()
                Keys.onEscapePressed: {
                    // Esc clears the value and collapses back to the
                    // button, exactly like the reference's searchKeydown.
                    drawer.query = ""
                    drawer.searchActive = false
                    searchField.focus = false
                    drawer.focus = false
                }
            }
        }
    }

    // ---------------------------------------------------------------------
    // The body: list plus the item-info column. Hidden while closed.
    // ---------------------------------------------------------------------
    Item {
        id: body
        anchors.top: tabStrip.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: 10
        anchors.rightMargin: 10
        anchors.bottomMargin: 10
        visible: drawer.open
        clip: true

        // Item info, the reference's rightPanelDrawer role (fixed here;
        // no third persisted width until categories exist).
        Rectangle {
            id: infoColumn
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.right: parent.right
            width: Math.min(220, parent.width * 0.28)
            color: "#242424"  // --primary-darkest
            border.color: "#4a4a4a"

            Column {
                anchors.fill: parent
                anchors.margins: 10
                spacing: 6

                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    color: "#b0b0b0"
                    font.pixelSize: 11
                    font.bold: true
                    text: drawer.tab === "shows" ? "SONG INFO" : "MEDIA INFO"
                }

                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    color: "#eeeeee"
                    font.pixelSize: 13
                    text: drawer.tab === "shows"
                        ? (docs.currentSongId === "" ? "nothing selected"
                           : docs.songs.filter(s => s.id === docs.currentSongId)
                                  .map(s => s.name).join(""))
                        : (docs.assets.filter(a => a.id === drawer.selectedAssetId)
                                  .map(a => a.name).join("") || "nothing selected")
                }

                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    color: "#b0b0b0"
                    font.pixelSize: 11
                    text: drawer.tab === "shows"
                          ? (docs.slides.length + " slides \u00b7 "
                             + "click selects \u00b7 double-click adds to the playlist")
                          : (drawer.editingEnabled ? docs.assets.filter(a => a.id === drawer.selectedAssetId)
                                    .map(a => a.sourcePath).join("") : "Select media to inspect it")
                }
            }
        }

        // --- Shows tab -------------------------------------------------
        Item {
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: infoColumn.left
            anchors.rightMargin: 10
            visible: drawer.tab === "shows"

            Flow {
                id: showActions
                visible: drawer.editingEnabled
                height: visible ? implicitHeight : 0
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                spacing: 6

                PanelField {
                    id: newSongName
                    width: Math.max(180, showActions.width - newSongButton.width - removeSongButton.width - 12)
                    placeholderText: "new show name"
                    placeholderTextColor: "#929292"
                    color: "#eeeeee"
                    onAccepted: newSongButton.clicked()
                }

                PanelButton {
                    id: newSongButton
                    text: "New show"
                    enabled: newSongName.text.trim().length > 0
                    onClicked: if (docs.createSong(newSongName.text)) newSongName.text = ""
                }

                PanelButton {
                    id: removeSongButton
                    text: "Remove"
                    enabled: docs.currentSongId !== ""
                    onClicked: docs.removeCurrentSong()
                }
            }

            Rectangle {
                anchors.top: showActions.bottom
                anchors.topMargin: 8
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                color: "#242424"
                border.color: "#4a4a4a"

                ListView {
                    id: showList
                    anchors.fill: parent
                    anchors.margins: 1
                    clip: true
                    // A blank query is the full library; anything else runs
                    // the reference's search over names and lyrics.
                    model: drawer.query === "" ? docs.songs : docs.searchShows(drawer.query)

                    delegate: Item {
                        id: showRow
                        objectName: "libraryShowRow"

                        required property var modelData
                        required property int index
                        width: showList.width
                        height: 28

                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: 1
                            color: showRow.modelData.id === docs.currentSongId ? "#1685f8"
                                 : (rowMouse.containsMouse ? "#4a4a4a" : "transparent")
                        }

                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            width: showRow.width - 80
                            elide: Text.ElideRight
                            font.pixelSize: 13
                            color: showRow.modelData.id === docs.currentSongId ? "#ffffff" : "#eeeeee"
                            text: showRow.modelData.name
                        }

                        Text {
                            anchors.right: parent.right
                            anchors.rightMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            font.pixelSize: 11
                            color: showRow.modelData.id === docs.currentSongId ? "#d9ebff" : "#b0b0b0"
                            text: showRow.modelData.slideCount
                                  + (showRow.modelData.slideCount === 1 ? " slide" : " slides")
                        }

                        MouseArea {
                            id: rowMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            drag.target: showDragProxy
                            acceptedButtons: Qt.LeftButton
                            onClicked: docs.selectSong(showRow.modelData.id)
                            onDoubleClicked: docs.addShowToPlaylist(showRow.modelData.id)
                            // Release delivers the drag wherever the pointer
                            // ended up (the playlist accepts it); releasing
                            // over the list is a no-op drop.
                            onReleased: {
                                if (showDragProxy.Drag.active) showDragProxy.Drag.drop()
                                showDragProxy.x = 0
                                showDragProxy.y = 0
                            }
                            onCanceled: { showDragProxy.x = 0; showDragProxy.y = 0 }
                        }

                        // The row drags out to the playlist as text/plain
                        // "show:<id>" — the drop side reads it back.
                        Item {
                            id: showDragProxy
                            readonly property string payload: "show:" + showRow.modelData.id
                            Drag.source: showDragProxy
                            width: showRow.width; height: showRow.height
                            Drag.active: rowMouse.drag.active
                            Drag.keys: ["text/plain"]
                            Drag.mimeData: ({"text/plain": "show:" + showRow.modelData.id})
                            Drag.hotSpot: Qt.point(width / 2, height / 2)
                        }
                    }

                    Text {
                        anchors.centerIn: parent
                        visible: showList.count === 0
                        color: "#929292"
                        font.pixelSize: 13
                        text: drawer.query === "" ? (drawer.editingEnabled ? "No presentations — create one above" : "No presentations — open Edit to create one")
                                                  : "nothing matches \u201c" + drawer.query + "\u201d"
                    }
                }
            }
        }

        // --- Media tab ---------------------------------------------------
        Item {
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: infoColumn.left
            anchors.rightMargin: 10
            visible: drawer.tab === "media"

            Flow {
                id: mediaActions
                anchors.top: parent.top
                anchors.left: parent.left
                spacing: 6

                PanelButton {
                    text: "Import\u2026"
                    onClicked: mediaDialog.open()
                }

                PanelButton {
                    text: "Remove"
                    visible: drawer.editingEnabled
                    enabled: drawer.selectedAssetId !== ""
                    onClicked: {
                        docs.removeAsset(drawer.selectedAssetId)
                        drawer.selectedAssetId = ""
                    }
                }

                // Task 6.17b2: put the selected asset on the slide under
                // edit as a new topmost picture — the picture's path from
                // this drawer is what "New box" is for words. A refusal
                // (no current slide, a video, the stack's ceiling) lands
                // on the status line naming itself; the drawer stays put.
                PanelButton {
                    objectName: "placeAssetButton"
                    visible: drawer.editingEnabled
                    text: "Add to slide"
                    enabled: drawer.selectedAssetId !== ""
                    onClicked: docs.placeAssetOnSlide(drawer.selectedAssetId)
                }
            }

            Rectangle {
                anchors.top: mediaActions.bottom
                anchors.topMargin: 8
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                color: "#242424"
                border.color: "#4a4a4a"

                ListView {
                    id: assetList
                    anchors.fill: parent
                    anchors.margins: 1
                    clip: true
                    model: docs.assets

                    delegate: Item {
                        id: assetRow

                        required property var modelData
                        width: assetList.width
                        height: 28

                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: 1
                            color: assetRow.modelData.id === drawer.selectedAssetId ? "#1685f8"
                                 : (assetMouse.containsMouse ? "#4a4a4a" : "transparent")
                        }

                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            width: Math.max(0, assetRow.width - (drawer.editingEnabled ? 200 : 16))
                            elide: Text.ElideRight
                            font.pixelSize: 13
                            color: assetRow.modelData.id === drawer.selectedAssetId ? "#ffffff" : "#eeeeee"
                            text: assetRow.modelData.name
                        }

                        Text {
                            anchors.right: parent.right
                            anchors.rightMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            width: 190
                            elide: Text.ElideMiddle
                            font.pixelSize: 11
                            color: assetRow.modelData.id === drawer.selectedAssetId ? "#d9ebff" : "#b0b0b0"
                            visible: drawer.editingEnabled
                            text: assetRow.modelData.sourcePath
                        }

                        MouseArea {
                            id: assetMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            acceptedButtons: Qt.LeftButton
                            onClicked: drawer.selectedAssetId = assetRow.modelData.id
                            // The shows tab's double-click rule, applied to
                            // media (6.17b2): the second click both selects
                            // the row and asks for it on the slide, so the
                            // picture lands selected and draggable.
                            onDoubleClicked: {
                                drawer.selectedAssetId = assetRow.modelData.id
                                if (drawer.editingEnabled) docs.placeAssetOnSlide(assetRow.modelData.id)
                            }
                        }
                    }

                    Text {
                        anchors.centerIn: parent
                        visible: assetList.count === 0
                        color: "#929292"
                        font.pixelSize: 13
                        text: "no media \u2014 import a file"
                    }
                }
            }

            FileDialog {
                id: mediaDialog
                title: "Import media"
                modality: Qt.ApplicationModal
                nameFilters: [
                    "Media files (*.jpg *.jpeg *.png *.webp *.gif *.mp4 *.mov *.mkv "
                    + "*.mp3 *.wav *.ogg)",
                    "All files (*)"
                ]
                onAccepted: docs.importMediaUrl(selectedFile)
            }
        }
    }

    // ---------------------------------------------------------------------
    // The top edge drags the height (the reference's Resizeable on the
    // drawer); WorkspacePanels' heightFor does the snapping.
    // ---------------------------------------------------------------------
    MouseArea {
        id: edgeDrag
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 6
        cursorShape: Qt.SizeVerCursor
        property real pressOffset: 0

        onPressed: function (mouse) {
            const y = mapToItem(null, 0, mouse.y).y
            pressOffset = drawer.parent.height - drawer.height - y
        }
        onPositionChanged: function (mouse) {
            if (!pressed) return
            const y = mapToItem(null, 0, mouse.y).y
            // maxHeight is the window minus the 40px top bar, like the
            // reference's available height.
            panels.resizeDrawer(drawer.parent.height - y - pressOffset,
                                drawer.parent.height - 64 - 300)
        }
    }
}
