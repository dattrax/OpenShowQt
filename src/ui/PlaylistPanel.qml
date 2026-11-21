import QtQuick
import QtQuick.Controls

// The sidebar host (tasks SL.4, SL.7; folder strip). One browser fills the
// pane: the top strip lists the FOLDERS (Songs, Presentations, Service) and
// never their files, the chosen folder's files fill the pane below it, and
// the title search sits at the bottom. Selecting, filtering and dragging
// never present anything.
SidePanel {
    id: playlistPanel
    side: "left"
    property bool editingEnabled: false
    width: panels.leftWidth
    onDraggedTo: candidate => panels.resizeLeft(candidate)
    onStripClicked: panels.clickLeft()

    // Browsing: a presentation (or a linked song's service show) is shown off
    // air in the middle of the screen. Nothing here presents.
    signal sourceChosen(string kind, string id)
    signal serviceEntryChosen(string entryId)
    signal songChosen(string id)

    readonly property bool libraryListFocused: libraryBrowser.libraryListFocused
    readonly property bool serviceListFocused: libraryBrowser.serviceListFocused
    readonly property string selectedFolder: libraryBrowser.selectedFolder
    function undoLibrary() { libraryBrowser.undoFocused() }
    function redoLibrary() { libraryBrowser.redoFocused() }

    // The selected cue lives with the Service folder; the host mirrors it so
    // other parts of the workspace can read one identity.
    readonly property string selectedEntryId: libraryBrowser.selectedEntryId

    // "Use service default": return the item to inheritance without a
    // measuring pass. A linked Library song also forgets its remembered look,
    // because "service default" is the whole-item unassignment.
    function useServiceDefault(showId) {
        if (showId === "")
            return
        const linked = docs.showInfo(showId).catalogItemId || ""
        if (!docs.clearWholeItemTheme(showId))
            return
        if (linked !== "")
            library.forgetItemTheme(linked)
    }

    function focusSearch() {
        libraryBrowser.focusSearch()
    }

    // Kept for callers that used UI.2's outline (Ctrl+F and the shell):
    // "library" means the Library pane, anything else the running order.
    function selectOutline(kind, id) {
        if (kind === "playlist" && id !== "")
            docs.selectPlaylist(id)
    }

    // Task PT.7: every whole-item apply goes through this one dialog. The
    // row supplies an explicit identity; the dialog captures it.
    ApplyThemeDialog {
        id: applyThemeDialog
    }

    Item {
        anchors.fill: parent

        LibraryBrowser {
            id: libraryBrowser
            objectName: "libraryBrowser"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: serviceDirText.top
            editingEnabled: playlistPanel.editingEnabled
            // An explicit add hands the operator the service it landed in.
            onServiceFocusRequested: playlistPanel.serviceEntryChosen("")
            onServiceEntryChosen: entryId => playlistPanel.serviceEntryChosen(entryId)
            onSongChosen: songId => playlistPanel.songChosen(songId)
            onPresentationChosen: showId => playlistPanel.sourceChosen("show", showId)
            onApplyThemeRequested: (showId, title) =>
                applyThemeDialog.openForShow(showId, title)
            onApplyCatalogThemeRequested: (itemId, title) =>
                applyThemeDialog.openForCatalogItem(itemId, title)
            onUseServiceDefaultRequested: showId =>
                playlistPanel.useServiceDefault(showId)
        }

        Text {
            id: serviceDirText
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.margins: 8
            wrapMode: Text.WordWrap
            color: docs.dirty ? "#d8c07a" : "#b0b0b0"
            font.pixelSize: 11
            text: docs.dirty ? "\u25cf Unsaved changes" : "No unsaved changes"
        }
    }
}
