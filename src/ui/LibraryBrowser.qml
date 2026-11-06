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
    property string undoScope: "catalog"
    function undoFocused() {
        if (undoScope !== "catalog") docs.undo()
        if (undoScope !== "shows") library.undo()
    }
    function redoFocused() {
        if (undoScope !== "shows") library.redo()
        if (undoScope !== "catalog") docs.redo()
    }
    signal serviceFocusRequested()
    function addSongToService(id) {
        if (songService.addToService(id)) browser.serviceFocusRequested()
    }
    readonly property var selectedLibraryKeys: selection.keys
    readonly property var visibleKeys: {
        let out = []
        if (songsHeader.songsExpanded)
            out = browser.songRows.map(row => "catalog:" + row.id)
        if (presentationsHeader.presentationsExpanded)
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
        }
    }


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

    readonly property real minimumUsefulHeight: 16 + libraryHeading.height + searchField.height
        + libraryActions.height + songsHeader.height + presentationsHeader.height
        + 6 * libraryContent.spacing + 56
    readonly property real listBudget: Math.max(0, libraryContent.height
        - libraryHeading.height - searchField.height - libraryActions.height
        - songsHeader.height - presentationsHeader.height - 6 * libraryContent.spacing)

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

    Column {
        id: libraryContent
        anchors.fill: parent
        anchors.margins: 8
        spacing: 4

        Row {
            id: libraryHeading
            width: parent.width
            height: 24
            Text {
                width: parent.width
                height: parent.height
                verticalAlignment: Text.AlignVCenter
                text: "LIBRARY"
                color: "#b0b0b0"; font.pixelSize: 11; font.bold: true
            }
        }

        PanelField {
            id: searchField
            objectName: "librarySearchField"
            width: parent.width
            fixedPlaceholderText: "Search library titles\u2026"
            placeholderTextColor: "#929292"
            text: browser.query
            onTextChanged: browser.query = text
            Keys.onEscapePressed: {
                browser.clearQuery()
                focus = false
            }
        }

        Flow {
            id: libraryActions
            visible: browser.editingEnabled
            height: visible ? implicitHeight : 0
            width: parent.width
            spacing: 4
            PanelButton {
                objectName: "libraryNewPresentationButton"
                width: 46
                height: 24
                text: "New"
                visible: browser.editingEnabled
                onClicked: libraryPanelNewDialog.open()
            }
        }

        // --- Songs (the shared catalog) --------------------------------
        Rectangle {
            id: songsHeader
            objectName: "librarySongsHeader"
            width: parent.width
            height: 26
            color: songsDrop.containsDrag ? "#164b78" : "transparent"
            border.color: "#65c4ff"
            border.width: songsDrop.containsDrag ? 2 : 0
            Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                text: (songsHeader.songsExpanded ? "\u25be " : "\u25b8 ") + "Songs"
                color: "#eeeeee"; font.pixelSize: 13; font.bold: true
            }
            Text {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: songsDrop.containsDrag ? "Move here" : browser.songRows.length + " / " + library.songCount
                color: "#b0b0b0"; font.pixelSize: 11
            }
            property bool songsExpanded: true
            MouseArea {
                anchors.fill: parent
                onClicked: songsHeader.songsExpanded = !songsHeader.songsExpanded
            }
            DropArea {
                id: songsDrop
                readonly property int dropAction: Qt.MoveAction
                readonly property string dropHint: "Move to Songs"
                anchors.fill: parent
                keys: ["text/plain"]
                onEntered: drag => {
                    const payload = drag.source ? String(drag.source.payload || "") : ""
                    drag.accepted = payload.startsWith("catalog:")
                    if (drag.accepted) drag.accept(Qt.MoveAction)
                }
                onDropped: function (drop) {
                    const payload = drop.source && drop.source.payload
                        ? drop.source.payload : drop.getDataAsString("text/plain")
                    if (!payload.startsWith("catalog:"))
                        return
                    if (library.moveItemToCategory(payload.substring(8), "songs"))
                        drop.accept(Qt.MoveAction)
                }
            }
        }
        ListView {
            id: songList
            objectName: "librarySongList"
            width: parent.width
            height: songsHeader.songsExpanded
                    ? Math.min(Math.max(1, browser.songRows.length) * 28 + 4,
                               browser.listBudget * (presentationsHeader.presentationsExpanded ? 0.5 : 1))
                    : 0
            visible: height > 0
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
                    hint: "Drag to the service or a category"
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

        // --- Presentations (this service's authored shows) --------------
        Rectangle {
            id: presentationsHeader
            objectName: "libraryPresentationsHeader"
            width: parent.width
            height: 26
            color: presentationsDrop.containsDrag ? "#164b78" : "transparent"
            border.color: "#65c4ff"
            border.width: presentationsDrop.containsDrag ? 2 : 0
            Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                text: (presentationsHeader.presentationsExpanded ? "\u25be " : "\u25b8 ") + "Presentations"
                color: "#eeeeee"; font.pixelSize: 13; font.bold: true
            }
            Text {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: presentationsDrop.containsDrag ? "Move here" : browser.presentationRows.length + " / " + docs.songs.length
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
                readonly property int dropAction: Qt.MoveAction
                readonly property string dropHint: "Move to Presentations"
                anchors.fill: parent
                keys: ["text/plain"]
                onEntered: drag => {
                    const payload = drag.source ? String(drag.source.payload || "") : ""
                    drag.accepted = payload.startsWith("catalog:")
                    if (drag.accepted) drag.accept(Qt.MoveAction)
                }
                onDropped: function (drop) {
                    const payload = drop.source && drop.source.payload
                        ? drop.source.payload : drop.getDataAsString("text/plain")
                    if (!payload.startsWith("catalog:"))
                        return
                    if (library.moveItemToCategory(payload.substring(8), "presentations"))
                        drop.accept(Qt.MoveAction)
                }
            }
        }
        ListView {
            id: libraryList
            objectName: "collectionLibraryList"
            width: parent.width
            height: presentationsHeader.presentationsExpanded
                    ? Math.max(0, browser.listBudget - songList.height)
                    : 0
            visible: height > 0
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
                            const at = libraryRow.mapToItem(null, mouse.x, mouse.y)
                            browser.themeMenuRequested(libraryRow.modelData.id, at.x, at.y)
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
                        else if (docs.addShowToPlaylist(libraryRow.modelData.id))
                            browser.serviceFocusRequested()
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
                    hint: "Drag to the service"
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
