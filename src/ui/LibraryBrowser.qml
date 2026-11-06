import QtQuick
import QtQuick.Controls

/*
 * UI part: LibraryBrowser (tasks SL.4, SL.5 and SL.7).
 *
 * The shared Library is the upper pane and stays visible while the running
 * order is edited below it. Two fixed branches — Songs (the shared catalog)
 * and Presentations (this service's authored shows) — are both expandable and
 * never replaced by a detail view. The search filters TITLES in both
 * branches; lyrics are never searched here.
 *
 * Rows are drag sources with a versioned payload:
 *   catalog:<id>  a Library song (category drops MOVE; service drops COPY a
 *                 reference-backed show)
 *   show:<id>     a presentation (service drops add it to the running order)
 * Category headers are drop targets for catalog moves. Selecting a row never
 * changes the running order or the audience.
 */
Item {
    id: browser
    objectName: "libraryBrowser"

    property bool editingEnabled: false
    property string query: ""
    property string selectedKind: ""
    property string selectedId: ""

    // The upper pane asks the shell for the import page; the page is the only
    // route that writes the catalog from pasted text.
    signal importRequested()
    // Browsing: a presentation can be shown off air; a song has no slide
    // document in the Library (its service projection, if any, is browsed
    // through the presentation id the shell resolves).
    signal presentationChosen(string id)
    signal songChosen(string id)
    // The host owns the one theme menu; the row supplies the item and the
    // scene coordinates to open it at.
    signal themeMenuRequested(string showId, real x, real y)

    function normalized(text) {
        return String(text).toLowerCase()
            .normalize("NFD").replace(/[\u0300-\u036f]/g, "")
            .replace(/[^a-z0-9 ]+/g, " ")
            .replace(/\s+/g, " ").trim()
    }
    function titleMatches(title, text) {
        const q = normalized(text)
        if (q === "")
            return true
        const t = normalized(title)
        const words = q.split(" ")
        for (let i = 0; i < words.length; ++i) {
            if (!new RegExp("(^| )" + words[i]).test(t))
                return false
        }
        return true
    }

    // Reading library.items / docs.songs is what makes these bindings follow
    // model signals; searchTitles() itself is a function call the engine
    // cannot observe.
    readonly property var songRows: {
        const all = library.items
        const matched = library.searchTitles(browser.query)
        const out = []
        for (let i = 0; i < matched.length; ++i) {
            if (matched[i].category === "songs")
                out.push(matched[i])
        }
        return out
    }
    readonly property var presentationRows: {
        const all = docs.songs
        const out = []
        for (let i = 0; i < all.length; ++i) {
            if (titleMatches(all[i].name, browser.query))
                out.push(all[i])
        }
        return out
    }

    function focusSearch() {
        Qt.callLater(() => searchField.forceActiveFocus())
    }
    function clearQuery() {
        browser.query = ""
        searchField.text = ""
    }

    // One menu serves every song row; the id is captured when it opens.
    property string contextSongId: ""
    property string renameSongId: ""

    Menu {
        id: songMenu
        objectName: "librarySongMenu"
        MenuItem {
            objectName: "libraryRenameSongItem"
            text: "Rename\u2026"
            onTriggered: {
                browser.renameSongId = browser.contextSongId
                const item = library.itemOf(browser.contextSongId)
                renameDialog.initialText = item.title || ""
                renameDialog.open()
                Qt.callLater(() => renameField.forceActiveFocus())
            }
        }
        MenuItem {
            objectName: "libraryMoveSongItem"
            text: "Move to Presentations"
            onTriggered: library.moveItemToCategory(browser.contextSongId,
                                                    "presentations")
        }
        MenuSeparator {}
        MenuItem {
            objectName: "libraryRemoveSongItem"
            text: "Remove from Library"
            onTriggered: library.removeItem(browser.contextSongId)
        }
    }

    // The dialog reads the id the menu wrote when it opened.
    Dialog {
        id: renameDialog
        objectName: "libraryRenameSongDialog"
        modal: true
        title: "Rename Library song"
        standardButtons: Dialog.Ok | Dialog.Cancel
        width: Math.min(420, browser.width - 40)
        property string initialText: ""
        onAccepted: {
            if (renameField.text.trim().length > 0 &&
                browser.renameSongId !== "")
                library.renameItem(browser.renameSongId, renameField.text)
            browser.renameSongId = ""
        }
        contentItem: Column {
            spacing: 8
            PanelField {
                id: renameField
                objectName: "renameSongField"
                width: parent.width
                text: renameDialog.initialText
            }
        }
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
                text: "LIBRARY"
                color: "#b0b0b0"; font.pixelSize: 11; font.bold: true
            }
            Item { width: Math.max(0, parent.width - 190); height: 1 }
            PanelButton {
                objectName: "libraryImportSongButton"
                width: 100
                height: 24
                text: "Import song"
                visible: browser.editingEnabled
                onClicked: browser.importRequested()
            }
            PanelButton {
                objectName: "libraryNewPresentationButton"
                width: 70
                height: 24
                text: "New"
                visible: browser.editingEnabled
                onClicked: libraryPanelNewDialog.open()
            }
        }

        PanelField {
            id: searchField
            objectName: "librarySearchField"
            width: parent.width
            placeholderText: "Search library titles\u2026"
            placeholderTextColor: "#929292"
            text: browser.query
            onTextChanged: browser.query = text
            Keys.onEscapePressed: {
                browser.clearQuery()
                focus = false
            }
        }

        // --- Songs (the shared catalog) --------------------------------
        Rectangle {
            id: songsHeader
            objectName: "librarySongsHeader"
            width: parent.width
            height: 26
            color: songsDrop.containsDrag ? "#1c4c7c" : "transparent"
            Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                text: (songsHeader.songsExpanded ? "\u25be " : "\u25b8 ") + "Songs"
                color: "#eeeeee"; font.pixelSize: 13; font.bold: true
            }
            Text {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: browser.songRows.length + " / " + library.songCount
                color: "#b0b0b0"; font.pixelSize: 11
            }
            property bool songsExpanded: true
            MouseArea {
                anchors.fill: parent
                onClicked: songsHeader.songsExpanded = !songsHeader.songsExpanded
            }
            DropArea {
                id: songsDrop
                anchors.fill: parent
                keys: ["text/plain"]
                onEntered: drag => {
                    const payload = drag.source ? String(drag.source.payload || "") : ""
                    if (payload.startsWith("catalog:"))
                        drag.accept(Qt.MoveAction)
                }
                onDropped: function (drop) {
                    const payload = drop.source && drop.source.payload
                        ? drop.source.payload : drop.getDataAsString("text/plain")
                    if (!payload.startsWith("catalog:"))
                        return
                    drop.accept()
                    library.moveItemToCategory(payload.substring(8), "songs")
                }
            }
        }
        ListView {
            id: songList
            objectName: "librarySongList"
            width: parent.width
            height: songsHeader.songsExpanded
                    ? Math.min(Math.max(1, browser.songRows.length) * 28 + 4,
                               Math.max(56, browser.height * 0.32))
                    : 0
            visible: height > 0
            clip: true
            model: browser.songRows
            ScrollBar.vertical: ScrollBar {}

            delegate: Item {
                id: songRow
                required property var modelData
                objectName: "librarySongRow"
                width: songList.width
                height: 28
                readonly property string songId: modelData.id
                readonly property bool rowSelected:
                    browser.selectedKind === "song" && browser.selectedId === modelData.id
                Accessible.name: "Song: " + modelData.name

                Rectangle {
                    anchors.fill: parent
                    anchors.margins: 1
                    color: songRow.rowSelected ? "#1685f8"
                         : (songMouse.containsMouse ? "#4a4a4a" : "transparent")
                }
                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 16
                    anchors.verticalCenter: parent.verticalCenter
                    width: songRow.width - 70
                    elide: Text.ElideRight
                    font.pixelSize: 13
                    color: songRow.rowSelected ? "#ffffff" : "#eeeeee"
                    text: modelData.title
                }
                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    font.pixelSize: 11
                    color: songRow.rowSelected ? "#d9ebff" : "#b0b0b0"
                    text: modelData.ccli !== "" ? "CCLI " + modelData.ccli : ""
                }
                MouseArea {
                    id: songMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    drag.target: songDrag
                    property bool leftHeld: false
                    onClicked: function (mouse) {
                        if (mouse.button === Qt.RightButton) {
                            browser.contextSongId = songRow.modelData.id
                            const at = songRow.mapToItem(browser, mouse.x, mouse.y)
                            songMenu.popup(at.x, at.y)
                            return
                        }
                        browser.selectedKind = "song"
                        browser.selectedId = songRow.modelData.id
                        browser.songChosen(songRow.modelData.id)
                    }
                    onDoubleClicked: songService.addToService(songRow.modelData.id)
                    onPressed: songMouse.leftHeld = mouse.button === Qt.LeftButton
                    onReleased: {
                        if (songMouse.leftHeld && songDrag.Drag.active)
                            songDrag.Drag.drop()
                        songMouse.leftHeld = false
                        songDrag.x = 0
                        songDrag.y = 0
                    }
                    onCanceled: { songMouse.leftHeld = false; songDrag.x = 0; songDrag.y = 0 }
                }
                Item {
                    id: songDrag
                    readonly property string payload: "catalog:" + songRow.modelData.id
                    Drag.source: songDrag
                    width: songRow.width; height: songRow.height
                    Drag.active: songMouse.drag.active
                    Drag.keys: ["text/plain"]
                    Drag.mimeData: ({"text/plain": songDrag.payload})
                    Drag.hotSpot: Qt.point(width / 2, height / 2)
                }
            }
            Text {
                anchors.centerIn: parent
                visible: songList.count === 0
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                color: "#929292"; font.pixelSize: 12
                text: browser.query !== ""
                      ? "no song titles match \u201c" + browser.query + "\u201d"
                      : (browser.editingEnabled
                         ? "No songs \u2014 Import song to add one"
                         : "No songs in the Library")
            }
        }

        // --- Presentations (this service's authored shows) --------------
        Rectangle {
            id: presentationsHeader
            objectName: "libraryPresentationsHeader"
            width: parent.width
            height: 26
            color: presentationsDrop.containsDrag ? "#1c4c7c" : "transparent"
            Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                text: (presentationsHeader.presentationsExpanded ? "\u25be " : "\u25b8 ") + "Presentations"
                color: "#eeeeee"; font.pixelSize: 13; font.bold: true
            }
            Text {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: browser.presentationRows.length + " / " + docs.songs.length
                color: "#b0b0b0"; font.pixelSize: 11
            }
            property bool presentationsExpanded: true
            MouseArea {
                anchors.fill: parent
                onClicked: presentationsHeader.presentationsExpanded =
                           !presentationsHeader.presentationsExpanded
            }
            DropArea {
                id: presentationsDrop
                anchors.fill: parent
                keys: ["text/plain"]
                onEntered: drag => {
                    const payload = drag.source ? String(drag.source.payload || "") : ""
                    if (payload.startsWith("catalog:"))
                        drag.accept(Qt.MoveAction)
                }
                onDropped: function (drop) {
                    const payload = drop.source && drop.source.payload
                        ? drop.source.payload : drop.getDataAsString("text/plain")
                    if (!payload.startsWith("catalog:"))
                        return
                    drop.accept()
                    library.moveItemToCategory(payload.substring(8), "presentations")
                }
            }
        }
        ListView {
            id: libraryList
            objectName: "collectionLibraryList"
            width: parent.width
            height: presentationsHeader.presentationsExpanded
                    ? Math.max(56, browser.height - 140 - songList.height)
                    : 0
            visible: height > 0
            clip: true
            model: browser.presentationRows
            ScrollBar.vertical: ScrollBar {}

            delegate: Item {
                id: libraryRow
                required property var modelData
                objectName: "collectionLibraryRow"
                width: libraryList.width
                height: 28
                readonly property string rowShowId: modelData.id
                readonly property bool rowSelected:
                    browser.selectedKind === "presentation"
                    && browser.selectedId === modelData.id
                Accessible.name: "Presentation: " + modelData.name

                Rectangle {
                    anchors.fill: parent
                    anchors.margins: 1
                    color: libraryRow.rowSelected ? "#1685f8"
                         : (rowMouse.containsMouse ? "#4a4a4a" : "transparent")
                }
                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 16
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
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    drag.target: libraryDragProxy
                    property bool leftHeld: false
                    onClicked: function (mouse) {
                        if (mouse.button === Qt.RightButton) {
                            const at = libraryRow.mapToItem(null, mouse.x, mouse.y)
                            browser.themeMenuRequested(libraryRow.modelData.id,
                                                       at.x, at.y)
                            return
                        }
                        browser.selectedKind = "presentation"
                        browser.selectedId = libraryRow.modelData.id
                        docs.selectSong(libraryRow.modelData.id)
                        browser.presentationChosen(libraryRow.modelData.id)
                    }
                    onPressed: rowMouse.leftHeld = mouse.button === Qt.LeftButton
                    onDoubleClicked: docs.addShowToPlaylist(libraryRow.modelData.id)
                    onReleased: {
                        if (rowMouse.leftHeld && libraryDragProxy.Drag.active)
                            libraryDragProxy.Drag.drop()
                        rowMouse.leftHeld = false
                        libraryDragProxy.x = 0
                        libraryDragProxy.y = 0
                    }
                    onCanceled: {
                        rowMouse.leftHeld = false
                        libraryDragProxy.x = 0
                        libraryDragProxy.y = 0
                    }
                }
                Item {
                    id: libraryDragProxy
                    readonly property string payload: "show:" + libraryRow.modelData.id
                    Drag.source: libraryDragProxy
                    width: libraryRow.width; height: libraryRow.height
                    Drag.active: rowMouse.drag.active
                    Drag.keys: ["text/plain"]
                    Drag.mimeData: ({"text/plain": libraryDragProxy.payload})
                    Drag.hotSpot: Qt.point(width / 2, height / 2)
                }
            }
            Text {
                anchors.centerIn: parent
                visible: libraryList.count === 0
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                color: "#929292"; font.pixelSize: 12
                text: browser.query !== ""
                      ? "no presentation titles match \u201c" + browser.query + "\u201d"
                      : (browser.editingEnabled
                         ? "No presentations \u2014 New to create one"
                         : "No presentations \u2014 open Edit to create one")
            }
        }
    }

    // The "New presentation" button's tiny dialog (kept from UI.4a so the
    // presentation branch still owns its creation action).
    Dialog {
        id: libraryPanelNewDialog
        objectName: "libraryNewPresentationDialog"
        modal: true
        title: "New presentation"
        standardButtons: Dialog.Ok | Dialog.Cancel
        width: Math.min(420, browser.width - 40)
        onAccepted: {
            if (newPresentationField.text.trim().length > 0)
                docs.createSong(newPresentationField.text.trim())
            newPresentationField.text = ""
        }
        contentItem: Column {
            spacing: 8
            PanelField {
                id: newPresentationField
                objectName: "libraryNewPresentationName"
                width: parent.width
                placeholderText: "Presentation name"
                placeholderTextColor: "#929292"
            }
        }
    }
}
