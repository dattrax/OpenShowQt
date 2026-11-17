import QtQuick
import QtQuick.Controls

/*
 * UI part: LibraryBrowser (tasks SL.4, SL.5, SL.7; folder strip).
 *
 * The sidebar's browser. The top strip is a list of FOLDERS — Songs (the
 * shared catalog), Presentations (this service's authored shows, plus catalog
 * records categorised there) and Service (the running order under
 * preparation). No folder spills its files into that strip: choosing one
 * lists its files in the pane below, so the strip stays the same three rows
 * however full the Library gets. The strip collapses when the operator wants
 * the files' room instead, and the title search sits at the bottom of the
 * browser.
 *
 * Rows are drag sources with a versioned payload:
 *   catalog:<id>  a Library song (folder drops MOVE; the Service folder COPYs
 *                 a reference-backed show)
 *   show:<id>     a presentation (the Service folder adds it to the order)
 * Folder rows are the drop targets: Songs and Presentations reclassify a
 * catalog item, Service prepares one. Selecting a row never changes the
 * running order or the audience.
 */
Item {
    id: browser
    objectName: "libraryBrowser"

    property bool editingEnabled: false
    property string query: ""
    property string selectedKind: ""
    property string selectedId: ""
    property string undoScope: "catalog"
    // The folder whose files the pane below lists: "songs", "presentations"
    // or "service". Folders never list their files inline.
    property string selectedFolder: "songs"
    property bool foldersExpanded: true

    readonly property bool libraryListFocused:
        browser.activeFocus || songList.activeFocus || libraryList.activeFocus
    readonly property bool serviceListFocused: servicePanel.activeFocus
    readonly property string selectedEntryId: servicePanel.selectedEntryId
    signal serviceEntryChosen(string entryId)
    signal serviceFocusRequested()

    function playlistName() {
        const rows = docs.playlists
        for (let i = 0; i < rows.length; ++i)
            if (rows[i].id === docs.currentPlaylistId) return rows[i].name
        return ""
    }
    // Choosing a folder moves the pane, and the keyboard with it: the list
    // the operator just asked for is the one Delete and Ctrl+A act on.
    function selectFolder(name) {
        if (name !== "songs" && name !== "presentations" && name !== "service")
            return
        browser.selectedFolder = name
        Qt.callLater(() => {
            if (name === "service") servicePanel.forceActiveFocus()
            else browser.forceActiveFocus()
        })
    }
    // An explicit add (menu, double-click) lands the operator on the result;
    // a drag onto the Service folder does not, so the source list stays put.
    function showService() {
        browser.selectedFolder = "service"
        Qt.callLater(() => servicePanel.forceActiveFocus())
    }
    // A payload dropped on a folder. Songs/Presentations reclassify a catalog
    // item; Service prepares whatever was dropped at the end of the order.
    function dropOnFolder(folder, payload) {
        if (payload.startsWith("catalog:")) {
            const id = payload.substring("catalog:".length)
            if (folder === "service") return songService.addToService(id)
            return library.moveItemToCategory(id, folder)
        }
        if (payload.startsWith("show:")) {
            if (folder === "service")
                return docs.addShowToPlaylist(payload.substring("show:".length))
            return false
        }
        if (payload.startsWith("media:")) {
            if (folder === "service")
                return docs.addMediaCue(payload.substring("media:".length),
                                        docs.playlistEntries.length)
            return false
        }
        return false
    }
    function undoFocused() {
        if (undoScope !== "catalog") docs.undo()
        if (undoScope !== "shows") library.undo()
    }
    function redoFocused() {
        if (undoScope !== "shows") library.redo()
        if (undoScope !== "catalog") docs.redo()
    }
    function addSongToService(id) {
        if (songService.addToService(id)) {
            browser.showService()
            browser.serviceFocusRequested()
        }
    }
    function addPresentationToService(id) {
        if (docs.addShowToPlaylist(id)) {
            browser.showService()
            browser.serviceFocusRequested()
        }
    }
    readonly property var selectedLibraryKeys: selection.keys
    // Every key the title search keeps, across BOTH library folders: the
    // folders are a view, not a filter, so Ctrl+A and a retained selection
    // still reach a row that is currently behind the other folder.
    readonly property var visibleKeys: {
        let out = browser.songRows.map(row => "catalog:" + row.id)
        out = out.concat(browser.presentationRows.map(row => row.rowKind + ":" + row.id))
        return out
    }
    ListSelection {
        id: selection
        onKeysChanged: browser.syncPrimarySelection()
    }
    onVisibleKeysChanged: if (selection) {
        selection.retain(visibleKeys)
        browser.syncPrimarySelection()
    }
    function syncPrimarySelection() {
        if (!selection) return
        const key = selection.contains(selection.currentKey) ? selection.currentKey
                    : (selection.keys.length ? selection.keys[0] : "")
        const colon = key.indexOf(":")
        selectedKind = key.startsWith("catalog:") ? "song" : (key ? "presentation" : "")
        selectedId = colon >= 0 ? key.substring(colon + 1) : ""
    }
    function selectRow(key, modifiers) {
        selection.select(key, visibleKeys, modifiers)
        browser.syncPrimarySelection()
    }
    function removeSelected() {
        if (!selection.keys.length || deleteConfirmation.visible) return
        deleteConfirmation.pendingKeys = selection.keys.slice()
        deleteConfirmation.open()
    }
    function deleteConfirmed(keys) {
        const catalogIds = [], showIds = []
        const catalogKeys = library.items.map(row => "catalog:" + row.id)
        const showKeys = docs.songs.map(row => "show:" + row.id)
        // Preflight both stores before the durable catalog batch. Catalog
        // notifications only detach linked songs, never authored show IDs.
        for (const key of keys) {
            if (catalogKeys.indexOf(key) >= 0) catalogIds.push(key.substring(8))
            else if (showKeys.indexOf(key) >= 0) showIds.push(key.substring(5))
            else { selection.retain(visibleKeys); return }
        }
        if (catalogIds.length && !library.removeItems(catalogIds, true)) return
        if (showIds.length && !docs.removeLibraryShows(showIds, true)) return
        if (keys.length) undoScope = catalogIds.length && showIds.length ? "mixed"
                                     : (showIds.length ? "shows" : "catalog")
        selection.clear()
    }

    Dialog {
        id: deleteConfirmation
        objectName: "libraryDeleteConfirmation"
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(460, parent.width - 32)
        implicitHeight: deleteWarning.implicitHeight + topPadding + bottomPadding
            + header.implicitHeight + footer.implicitHeight + 2 * spacing
        modal: true
        title: "Delete from Library?"
        background: Rectangle { color: "#333333"; radius: 8; border.color: "#606060" }
        Overlay.modal: Rectangle { color: "#99000000" }
        property var pendingKeys: []
        onOpened: cancelDeleteButton.forceActiveFocus()
        footer: DialogButtonBox {
            Button {
                id: cancelDeleteButton
                objectName: "libraryCancelDeleteButton"
                text: "Cancel"
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
                Keys.onReturnPressed: deleteConfirmation.reject()
                Keys.onEnterPressed: deleteConfirmation.reject()
            }
            Button {
                objectName: "libraryConfirmDeleteButton"
                text: "Delete permanently"
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
                Keys.onReturnPressed: deleteConfirmation.accept()
                Keys.onEnterPressed: deleteConfirmation.accept()
            }
        }
        onAccepted: {
            const keys = pendingKeys.slice()
            pendingKeys = []
            browser.deleteConfirmed(keys)
        }
        onRejected: pendingKeys = []
        onClosed: browser.forceActiveFocus()
        contentItem: Item {
            implicitWidth: 400
            implicitHeight: deleteWarning.implicitHeight
            Text {
                id: deleteWarning
                objectName: "libraryDeleteWarningText"
                width: parent.width
                color: "#eeeeee"
                font.pixelSize: 14
                text: "Permanently delete " + deleteConfirmation.pendingKeys.length
                    + (deleteConfirmation.pendingKeys.length === 1 ? " item" : " items")
                    + " from the song/presentation database? This cannot be undone."
                    + "\n\nCopies already in the current service will be kept."
                wrapMode: Text.WordWrap
            }
        }
    }
    Keys.enabled: browser.activeFocus
    Keys.onPressed: event => {
        if (event.key === Qt.Key_Delete) {
            browser.removeSelected()
            event.accepted = true
        } else if (event.key === Qt.Key_A && (event.modifiers & Qt.ControlModifier)) {
            selection.selectAll(browser.visibleKeys)
            browser.syncPrimarySelection()
            event.accepted = true
        } else if (event.key === Qt.Key_Menu ||
                   (event.key === Qt.Key_F10 &&
                    (event.modifiers & Qt.ShiftModifier))) {
            // The keyboard's context menu: open the same row menu the mouse
            // uses, so Apply theme… is reachable without a pointer.
            if (browser.selectedFolder === "songs" && songList.count > 0 &&
                songList.currentIndex >= 0) {
                const row = songList.itemAtIndex(songList.currentIndex)
                if (row) {
                    browser.contextSongId = browser.songRows[songList.currentIndex].id
                    const at = row.mapToItem(browser, row.width / 2, row.height / 2)
                    songMenu.popup(at.x, at.y)
                }
            } else if (browser.selectedFolder === "presentations" &&
                       libraryList.count > 0 && libraryList.currentIndex >= 0) {
                const row = libraryList.itemAtIndex(libraryList.currentIndex)
                const model = browser.presentationRows[libraryList.currentIndex]
                if (row && model) {
                    browser.applyThemeRequested(model.id, model.name || "")
                }
            }
            event.accepted = true
        }
    }


    // Browsing: a presentation can be shown off air; a song has no slide
    // document in the Library (its service projection, if any, is browsed
    // through the presentation id the shell resolves).
    signal presentationChosen(string id)
    signal songChosen(string id)
    // Task PT.7: the host owns the ONE whole-item apply dialog. The row
    // supplies an explicit identity — a catalog record or a service show —
    // never a bare theme id or the editing cursor.
    signal applyThemeRequested(string showId, string title)
    signal applyCatalogThemeRequested(string itemId, string title)
    signal useServiceDefaultRequested(string showId)

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
    // The Presentations branch lists authored service shows AND catalog
    // records categorised as presentations (an operator move inside the
    // Library). Catalog rows carry rowKind "catalog" and keep the same drop
    // payloads as a song; a service row stays "show".
    readonly property var presentationRows: {
        const items = library.items
        const all = docs.songs
        const out = []
        for (let i = 0; i < items.length; ++i) {
            if (items[i].category === "presentations" &&
                titleMatches(items[i].title, browser.query)) {
                out.push({rowKind: "catalog", id: items[i].id,
                          name: items[i].title, slideCount: 0,
                          catalog: items[i]})
            }
        }
        for (let i = 0; i < all.length; ++i) {
            if (titleMatches(all[i].name, browser.query)) {
                const row = all[i]
                out.push({rowKind: "show", id: row.id, name: row.name,
                          slideCount: row.slideCount, catalog: null})
            }
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
            objectName: "libraryApplyThemeItem"
            text: "Apply theme\u2026"
            onTriggered: {
                const item = library.itemOf(browser.contextSongId)
                browser.applyCatalogThemeRequested(browser.contextSongId,
                                                   item.title || "")
            }
        }
        MenuSeparator {}
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
            objectName: "libraryAddSongToServiceItem"
            text: "Add to current service"
            onTriggered: browser.addSongToService(browser.contextSongId)
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
            text: "Delete from Library\u2026"
            onTriggered: browser.removeSelected()
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

    // One folder row: a name, the count that used to be an expanded header's
    // tally, and the drop target that puts a file there. The strip never lists
    // a folder's files — choosing the row is what lists them, below.
    component FolderRow: Rectangle {
        id: folderRow
        property string folder: ""
        property string label: ""
        property string detail: ""
        property bool current: false
        property bool takesDecks: false
        property string dropHint: ""
        // The automation name of the row's drop target.
        property string dropName: ""
        // The payload prefixes this folder takes ("catalog:", "show:"…).
        property var accepted: []
        // True while a drag this folder would accept is over it (the row
        // says what the drop will do, and a test can ask).
        readonly property alias hovering: folderDrop.hovering

        signal chosen()
        signal fileDropped(string payload)
        signal deckDropped(string url)

        Accessible.role: Accessible.Button
        Accessible.name: folderRow.label + (folderRow.current ? ", open folder" : ", folder")
        width: parent ? parent.width : 0
        height: 26
        color: folderDrop.hovering ? "#164b78"
             : (current ? "#1685f8" : (rowMouse.containsMouse ? "#4a4a4a" : "transparent"))
        border.color: "#65c4ff"
        border.width: folderDrop.hovering ? 2 : 0

        Text {
            anchors.left: parent.left
            anchors.leftMargin: 8
            anchors.right: detailText.left
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            elide: Text.ElideRight
            text: (folderRow.current ? "\u25be " : "\u25b8 ") + folderRow.label
            color: "#eeeeee"; font.pixelSize: 13; font.bold: true
        }
        Text {
            id: detailText
            anchors.right: parent.right
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            text: folderDrop.hovering && folderRow.dropHint !== ""
                  ? folderRow.dropHint : folderRow.detail
            color: folderDrop.hovering || folderRow.current ? "#d9ebff" : "#b0b0b0"
            font.pixelSize: 11
        }
        MouseArea {
            id: rowMouse
            anchors.fill: parent
            hoverEnabled: true
            onClicked: folderRow.chosen()
        }
        DropArea {
            id: folderDrop
            objectName: folderRow.dropName
            anchors.fill: parent
            keys: ["text/plain", "text/uri-list"]
            property bool hovering: false
            // Qt does not expose an internal Quick drag's payload through
            // DragEvent.text, so the SOURCE is read first and the mime text
            // only answers a foreign drag — the workspace's one rule.
            function payloadOf(event) {
                const carried = event.source ? String(event.source.payload || "") : ""
                return carried !== "" ? carried
                                      : String(event.getDataAsString("text/plain") || "")
            }
            function acceptedPayload(payload) {
                for (let i = 0; i < folderRow.accepted.length; ++i)
                    if (payload.startsWith(folderRow.accepted[i])) return true
                return false
            }
            function acceptsDeck(drag) {
                if (!folderRow.takesDecks || docs.powerPointImportBusy) return false
                const urls = drag.urls
                return !!urls && urls.length === 1
                    && String(urls[0]).toLowerCase().endsWith(".pptx")
            }
            onEntered: drag => {
                const valid = drag.hasUrls ? acceptsDeck(drag)
                                           : acceptedPayload(payloadOf(drag))
                drag.accepted = valid
                folderDrop.hovering = valid
                if (valid)
                    drag.accept(drag.hasUrls ? Qt.CopyAction
                               : (folderRow.folder === "service" ? Qt.CopyAction
                                                                 : Qt.MoveAction))
            }
            onExited: folderDrop.hovering = false
            onDropped: function (drop) {
                folderDrop.hovering = false
                if (drop.hasUrls) {
                    if (!acceptsDeck(drop)) return
                    drop.accept(Qt.CopyAction)
                    folderRow.deckDropped(String(drop.urls[0]))
                    return
                }
                const payload = payloadOf(drop)
                if (!acceptedPayload(payload)) return
                drop.accept(folderRow.folder === "service" ? Qt.CopyAction
                                                           : Qt.MoveAction)
                folderRow.fileDropped(payload)
            }
        }
    }

    Item {
        anchors.fill: parent
        anchors.margins: 8

        Item {
            id: libraryHeading
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 24

            Text {
                anchors.left: parent.left
                anchors.right: foldersToggle.left
                anchors.rightMargin: 6
                height: parent.height
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
                text: "LIBRARY"
                color: "#b0b0b0"; font.pixelSize: 11; font.bold: true
            }
            // The folder strip is a view of three rows, never a list of every
            // file: collapsing it hands its room to the folder already open.
            Text {
                id: foldersToggle
                objectName: "libraryFoldersToggle"
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                width: 22
                horizontalAlignment: Text.AlignHCenter
                text: browser.foldersExpanded ? "\u25be" : "\u25b8"
                color: toggleMouse.containsMouse ? "#eeeeee" : "#b0b0b0"
                font.pixelSize: 12
                MouseArea {
                    id: toggleMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: browser.foldersExpanded = !browser.foldersExpanded
                }
                Accessible.name: browser.foldersExpanded ? "Collapse the folders"
                                                         : "Show the folders"
            }
        }

        // The folder strip is a LIST, not a dropdown: it wears the same
        // frame as the files below it, and both boxes are exactly the width
        // of the search field under them.
        Rectangle {
            id: folderBox
            objectName: "libraryFolderBox"
            visible: browser.foldersExpanded
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: libraryHeading.bottom
            height: folderStrip.implicitHeight + 2
            color: "#242424"
            border.color: "#4a4a4a"
            border.width: 1

            Column {
                id: folderStrip
                objectName: "libraryFolderStrip"
                anchors.fill: parent
                anchors.margins: 1
                spacing: 0

                FolderRow {
                    objectName: "librarySongsHeader"
                    folder: "songs"
                    label: "Songs"
                    detail: browser.songRows.length + " / " + library.songCount
                    current: browser.selectedFolder === "songs"
                    dropHint: "Move to Songs"
                    dropName: "librarySongsDrop"
                    accepted: ["catalog:"]
                    onChosen: browser.selectFolder("songs")
                    onFileDropped: payload => browser.dropOnFolder("songs", payload)
                }
                FolderRow {
                    objectName: "libraryPresentationsHeader"
                    folder: "presentations"
                    label: "Presentations"
                    detail: browser.presentationRows.length + " / " + docs.songs.length
                    current: browser.selectedFolder === "presentations"
                    dropHint: "Move here"
                    dropName: "presentationsDrop"
                    takesDecks: true
                    accepted: ["catalog:"]
                    onChosen: browser.selectFolder("presentations")
                    onFileDropped: payload => browser.dropOnFolder("presentations", payload)
                    onDeckDropped: url => {
                        if (docs.importPowerPointUrlToLibrary(url)) browser.forceActiveFocus()
                    }
                }
                FolderRow {
                    objectName: "libraryServiceHeader"
                    folder: "service"
                    label: "Service"
                    detail: (browser.playlistName() !== ""
                             ? browser.playlistName() + " \u00b7 " : "")
                            + docs.playlistEntries.length + " cues"
                    current: browser.selectedFolder === "service"
                    dropHint: "Add to service"
                    dropName: "libraryServiceDrop"
                    takesDecks: true
                    accepted: ["catalog:", "show:", "media:"]
                    onChosen: browser.selectFolder("service")
                    onFileDropped: payload => browser.dropOnFolder("service", payload)
                    onDeckDropped: url => {
                        if (docs.importPowerPointUrlToService(url,
                                                              docs.playlistEntries.length))
                            browser.forceActiveFocus()
                    }
                }
            }
        }

        // The title search is the LAST row of the browser, under whatever
        // folder is open — the same field Ctrl+F has always focused.
        PanelField {
            id: searchField
            objectName: "librarySearchField"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            fixedPlaceholderText: "Search library titles\u2026"
            placeholderTextColor: "#929292"
            text: browser.query
            onTextChanged: browser.query = text
            Keys.onEscapePressed: {
                browser.clearQuery()
                focus = false
            }
        }

        Rectangle {
            id: contentBox
            objectName: "libraryContentBox"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: browser.foldersExpanded ? folderBox.bottom
                                                 : libraryHeading.bottom
            anchors.topMargin: 6
            anchors.bottom: searchField.top
            anchors.bottomMargin: 6
            color: "#242424"
            border.color: "#4a4a4a"
            border.width: 1

            ListView {
                id: songList
                objectName: "librarySongList"
                anchors.fill: parent
                anchors.margins: 1
                visible: browser.selectedFolder === "songs"
                clip: true
                model: browser.songRows
                ScrollBar.vertical: ScrollBar {}

                delegate: Item {
                    id: songRow
                    required property int index
                    required property var modelData
                    objectName: "librarySongRow"
                    width: songList.width
                    height: 28
                    readonly property string songId: modelData.id
                    readonly property bool rowSelected:
                        selection.contains("catalog:" + modelData.id)
                    Accessible.name: "Song: " + modelData.title

                    CardChrome {
                        anchors.fill: parent
                        anchors.margins: 1
                        selected: songRow.rowSelected
                        hovered: songMouse.containsMouse
                        idleColor: "transparent"
                        hoverColor: "#4a4a4a"
                        selectedColor: "#1685f8"
                        selectedBorder: "#1685f8"
                        idleBorder: "transparent"
                        selectedBorderWidth: 1
                        borderWidth: 1
                        radius: 0
                    }
                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 16
                        anchors.verticalCenter: parent.verticalCenter
                        width: songRow.width - 24
                        elide: Text.ElideRight
                        font.pixelSize: 13
                        color: songRow.rowSelected ? "#ffffff" : "#eeeeee"
                        text: modelData.title
                    }
                    MouseArea {
                        id: songMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        preventStealing: true
                        cursorShape: drag.active ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        drag.smoothed: false
                        drag.target: songDrag
                        property bool leftHeld: false
                        onClicked: function (mouse) {
                            const key = "catalog:" + songRow.modelData.id
                            if (mouse.button === Qt.RightButton) {
                                if (!selection.contains(key)) browser.selectRow(key, Qt.NoModifier)
                                browser.contextSongId = songRow.modelData.id
                                const at = songRow.mapToItem(browser, mouse.x, mouse.y)
                                songMenu.popup(at.x, at.y)
                                return
                            }
                            browser.selectRow(key, mouse.modifiers)
                            browser.songChosen(songRow.modelData.id)
                        }
                        onDoubleClicked: browser.addSongToService(songRow.modelData.id)
                        onPressed: function(mouse) {
                            browser.forceActiveFocus()
                            browser.undoScope = "catalog"
                            songList.currentIndex = songRow.index
                            songDrag.begin(songRow, mouse.x, mouse.y)
                            songMouse.leftHeld = mouse.button === Qt.LeftButton
                        }
                        onReleased: {
                            if (songMouse.leftHeld && songDrag.Drag.active)
                                songDrag.Drag.drop()
                            songMouse.leftHeld = false
                            songDrag.x = 0
                            songDrag.y = 0
                        }
                        onCanceled: { songMouse.leftHeld = false; songDrag.x = 0; songDrag.y = 0 }
                    }
                    DragFeedback {
                        id: songDrag
                        payload: "catalog:" + songRow.modelData.id
                        title: songRow.modelData.title
                        hint: "Drag to the Service folder"
                        dragging: songMouse.drag.active && songMouse.leftHeld
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

            ListView {
                id: libraryList
                objectName: "collectionLibraryList"
                anchors.fill: parent
                anchors.margins: 1
                visible: browser.selectedFolder === "presentations"
                clip: true
                model: browser.presentationRows
                ScrollBar.vertical: ScrollBar {}

                delegate: Item {
                    id: libraryRow
                    required property int index
                    required property var modelData
                    objectName: "collectionLibraryRow"
                    width: libraryList.width
                    height: 28
                    readonly property string rowShowId: modelData.id
                    readonly property bool rowSelected:
                        selection.contains(modelData.rowKind + ":" + modelData.id)
                    Accessible.name: "Presentation: " + modelData.name

                    CardChrome {
                        anchors.fill: parent
                        anchors.margins: 1
                        selected: libraryRow.rowSelected
                        hovered: rowMouse.containsMouse
                        idleColor: "transparent"
                        hoverColor: "#4a4a4a"
                        selectedColor: "#1685f8"
                        selectedBorder: "#1685f8"
                        idleBorder: "transparent"
                        selectedBorderWidth: 1
                        borderWidth: 1
                        radius: 0
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
                        preventStealing: true
                        cursorShape: drag.active ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        drag.smoothed: false
                        drag.target: libraryDragProxy
                        property bool leftHeld: false
                        onClicked: function (mouse) {
                            const key = libraryRow.modelData.rowKind + ":" + libraryRow.modelData.id
                            if (mouse.button !== Qt.RightButton || !selection.contains(key))
                                browser.selectRow(key, mouse.button === Qt.RightButton ? Qt.NoModifier : mouse.modifiers)
                            if (libraryRow.modelData.rowKind === "catalog") {
                                if (mouse.button === Qt.RightButton) {
                                    browser.contextSongId = libraryRow.modelData.id
                                    const at = libraryRow.mapToItem(null, mouse.x, mouse.y)
                                    songMenu.popup(at.x, at.y)
                                } else browser.songChosen(libraryRow.modelData.id)
                                return
                            }
                            if (mouse.button === Qt.RightButton) {
                                browser.applyThemeRequested(
                                    libraryRow.modelData.id,
                                    libraryRow.modelData.name || "")
                            } else {
                                docs.selectSong(libraryRow.modelData.id)
                                browser.presentationChosen(libraryRow.modelData.id)
                            }
                        }
                        onPressed: function(mouse) {
                            browser.forceActiveFocus()
                            browser.undoScope = libraryRow.modelData.rowKind === "catalog" ? "catalog" : "shows"
                            libraryList.currentIndex = libraryRow.index
                            libraryDragProxy.begin(libraryRow, mouse.x, mouse.y)
                            rowMouse.leftHeld = mouse.button === Qt.LeftButton
                        }
                        onDoubleClicked: {
                            if (libraryRow.modelData.rowKind === "catalog")
                                browser.addSongToService(libraryRow.modelData.id)
                            else
                                browser.addPresentationToService(libraryRow.modelData.id)
                        }
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
                    DragFeedback {
                        id: libraryDragProxy
                        payload: (libraryRow.modelData.rowKind === "catalog"
                                  ? "catalog:" : "show:") + libraryRow.modelData.id
                        title: libraryRow.modelData.name
                        hint: "Drag to the Service folder"
                        dragging: rowMouse.drag.active && rowMouse.leftHeld
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
                          : "Drop PowerPoint files here"
                }
            }

            // The Service folder IS the running order: the same panel the
            // lower pane always was, with its own heading left to the folder
            // row that names it.
            CurrentServicePanel {
                id: servicePanel
                objectName: "currentServicePanel"
                anchors.fill: parent
                anchors.margins: 1
                visible: browser.selectedFolder === "service"
                showHeading: false
                editingEnabled: browser.editingEnabled
                onEntryChosen: entryId => browser.serviceEntryChosen(entryId)
                onApplyThemeRequested: (showId, title) =>
                    browser.applyThemeRequested(showId, title)
                onUseServiceDefaultRequested: showId =>
                    browser.useServiceDefaultRequested(showId)
            }
        }
    }
}
