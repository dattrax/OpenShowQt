import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs

/*
 * The library drawer (task 4.2, docs/reference.md §4): the bottom strip
 * the operator finds shows and media in. Height is WorkspacePanels'
 * (open 300, closed to a 40px tab bar; Ctrl+D or a click on the bar
 * toggles, the top edge drags with the reference's snap). The tab bar
 * carries the reference's eight tabs in order — Shows and Media are the
 * ones with content today, the rest wait for their milestones and sit
 * disabled — and at its right end the Search button (100px) swaps for
 * the 290px input on Ctrl+F; Esc clears and collapses it, and Enter
 * drops the first match into the playlist after the current show, like
 * the reference.
 *
 * The reference nests its own three-column split in here (categories,
 * list, item info). OpenShowQt has no tag categories yet, so the drawer
 * runs list + info, with the widths following the window rather than a
 * third persisted pair.
 */
Rectangle {
    id: drawer

    color: "#242832"  // --primary
    height: panels.drawerHeight

    readonly property bool open: panels.drawerHeight > 40.5
    property string tab: "shows"
    property string query: ""
    property bool searchActive: false
    property string selectedAssetId: ""

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
                    {id: "media", label: "Media", enabled: true},
                    {id: "audio", label: "Audio", enabled: false},
                    {id: "overlays", label: "Overlays", enabled: false},
                    {id: "templates", label: "Templates", enabled: false},
                    {id: "scripture", label: "Scripture", enabled: false},
                    {id: "calendar", label: "Calendar", enabled: false},
                    {id: "functions", label: "Functions", enabled: false}
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
                        color: tabButton.active ? "#f0f0ff"
                             : (tabButton.modelData.enabled ? "#8f93a5" : "#4a4f5e")
                        text: tabButton.modelData.label
                    }

                    Rectangle {
                        width: parent.width
                        height: 2
                        anchors.bottom: parent.bottom
                        visible: tabButton.active
                        color: "#f0008c"  // --secondary
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

            Button {
                id: searchButton
                visible: !drawer.searchActive
                width: 100
                height: 36
                text: "Search"
                onClicked: drawer.focusSearch()
            }

            TextField {
                id: searchField
                visible: drawer.searchActive
                width: 290
                height: 36
                placeholderText: "Search..."
                placeholderTextColor: "#6a6f80"
                color: "#f0f0ff"
                text: drawer.query
                onTextChanged: drawer.query = text
                onAccepted: drawer.searchEnter()
                Keys.onEscapePressed: {
                    // Esc clears the value and collapses back to the
                    // button, exactly like the reference's searchKeydown.
                    drawer.query = ""
                    drawer.searchActive = false
                    searchField.focus = false
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
            width: 260
            color: "#12121c"  // --primary-darkest
            border.color: "#2f3542"

            Column {
                anchors.fill: parent
                anchors.margins: 10
                spacing: 6

                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    color: "#8f93a5"
                    font.pixelSize: 11
                    font.bold: true
                    text: drawer.tab === "shows" ? "SONG INFO" : "MEDIA INFO"
                }

                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    color: "#f0f0ff"
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
                    color: "#8f93a5"
                    font.pixelSize: 11
                    text: drawer.tab === "shows"
                          ? (docs.slides.length + " slides \u00b7 "
                             + "click selects \u00b7 double-click adds to the playlist")
                          : (docs.assets.filter(a => a.id === drawer.selectedAssetId)
                                    .map(a => a.sourcePath).join("") || " ")
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

            Row {
                id: showActions
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                spacing: 6

                TextField {
                    id: newSongName
                    width: showActions.width - newSongButton.width - removeSongButton.width - 12
                    placeholderText: "new show name"
                    placeholderTextColor: "#6a6f80"
                    color: "#f0f0ff"
                    onAccepted: newSongButton.clicked()
                }

                Button {
                    id: newSongButton
                    text: "New show"
                    enabled: newSongName.text.trim().length > 0
                    onClicked: if (docs.createSong(newSongName.text)) newSongName.text = ""
                }

                Button {
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
                color: "#12121c"
                border.color: "#2f3542"

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

                        required property var modelData
                        required property int index
                        width: showList.width
                        height: 28

                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: 1
                            color: showRow.modelData.id === docs.currentSongId ? "#f0008c"
                                 : (rowMouse.containsMouse ? "#2f3542" : "transparent")
                        }

                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            width: showRow.width - 80
                            elide: Text.ElideRight
                            font.pixelSize: 13
                            color: showRow.modelData.id === docs.currentSongId ? "#ffffff" : "#f0f0ff"
                            text: showRow.modelData.name
                        }

                        Text {
                            anchors.right: parent.right
                            anchors.rightMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            font.pixelSize: 11
                            color: showRow.modelData.id === docs.currentSongId ? "#f8d0e6" : "#8f93a5"
                            text: showRow.modelData.slideCount
                                  + (showRow.modelData.slideCount === 1 ? " slide" : " slides")
                        }

                        MouseArea {
                            id: rowMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            drag.target: showRow
                            acceptedButtons: Qt.LeftButton
                            onClicked: docs.selectSong(showRow.modelData.id)
                            onDoubleClicked: docs.addShowToPlaylist(showRow.modelData.id)
                            // Release delivers the drag wherever the pointer
                            // ended up (the playlist accepts it); releasing
                            // over the list is a no-op drop.
                            onReleased: if (showRow.Drag.active) showRow.Drag.drop()
                        }

                        // The row drags out to the playlist as text/plain
                        // "show:<id>" — the drop side reads it back.
                        Drag.active: rowMouse.drag.active
                        Drag.keys: ["text/plain"]
                        Drag.mimeData: ({ "text/plain": "show:" + showRow.modelData.id })
                        Drag.hotSpot: Qt.point(width / 2, height / 2)
                    }

                    Text {
                        anchors.centerIn: parent
                        visible: showList.count === 0
                        color: "#6a6f80"
                        font.pixelSize: 13
                        text: drawer.query === "" ? "no songs \u2014 create one above"
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

            Row {
                id: mediaActions
                anchors.top: parent.top
                anchors.left: parent.left
                spacing: 6

                Button {
                    text: "Import\u2026"
                    onClicked: mediaDialog.open()
                }

                Button {
                    text: "Remove"
                    enabled: drawer.selectedAssetId !== ""
                    onClicked: {
                        docs.removeAsset(drawer.selectedAssetId)
                        drawer.selectedAssetId = ""
                    }
                }
            }

            Rectangle {
                anchors.top: mediaActions.bottom
                anchors.topMargin: 8
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                color: "#12121c"
                border.color: "#2f3542"

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
                            color: assetRow.modelData.id === drawer.selectedAssetId ? "#f0008c"
                                 : (assetMouse.containsMouse ? "#2f3542" : "transparent")
                        }

                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            width: assetRow.width - 200
                            elide: Text.ElideRight
                            font.pixelSize: 13
                            color: assetRow.modelData.id === drawer.selectedAssetId ? "#ffffff" : "#f0f0ff"
                            text: assetRow.modelData.name
                        }

                        Text {
                            anchors.right: parent.right
                            anchors.rightMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            width: 190
                            elide: Text.ElideMiddle
                            font.pixelSize: 11
                            color: assetRow.modelData.id === drawer.selectedAssetId ? "#f8d0e6" : "#8f93a5"
                            text: assetRow.modelData.sourcePath
                        }

                        MouseArea {
                            id: assetMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: drawer.selectedAssetId = assetRow.modelData.id
                        }
                    }

                    Text {
                        anchors.centerIn: parent
                        visible: assetList.count === 0
                        color: "#6a6f80"
                        font.pixelSize: 13
                        text: "no media \u2014 import a file"
                    }
                }
            }

            FileDialog {
                id: mediaDialog
                title: "Import media"
                nameFilters: [
                    "Media files (*.jpg *.jpeg *.png *.webp *.gif *.mp4 *.mov *.mkv "
                    + "*.mp3 *.wav *.ogg)",
                    "All files (*)"
                ]
                onAccepted: docs.importMediaFile(selectedFile.toLocalFile())
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
            pressOffset = drawer.parent.height - panels.drawerHeight - y
        }
        onPositionChanged: function (mouse) {
            const y = mapToItem(null, 0, mouse.y).y
            // maxHeight is the window minus the 40px top bar, like the
            // reference's available height.
            panels.resizeDrawer(drawer.parent.height - y - pressOffset,
                                drawer.parent.height - 40)
        }
    }
}
