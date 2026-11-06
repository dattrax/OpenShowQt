import QtQuick
import QtQuick.Controls

// The sidebar host (task SL.4). It no longer switches one detail view
// between the Library and a playlist: the shared Library is the upper pane
// and the current service's running order is the lower pane, both visible
// and independently scrollable. The Library comes from the catalog; the
// service rows are this service's shows. Selecting, filtering and dragging
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
    signal songChosen(string id)
    // The import page is the only route that writes the catalog from text.
    signal importRequested()

    // The presentation row the theme context menu is acting on (task TH.5).
    property string contextShowId: ""

    // The selected cue lives with the lower pane; the host mirrors it so
    // other parts of the workspace can read one identity.
    readonly property string selectedEntryId: servicePanel.selectedEntryId

    function focusSearch() {
        libraryBrowser.focusSearch()
        libraryBrowser.forceActiveFocus()
    }

    // Kept for callers that used UI.2's outline (Ctrl+F and the shell):
    // "library" means the Library pane, anything else the running order.
    function selectOutline(kind, id) {
        if (kind === "playlist" && id !== "")
            docs.selectPlaylist(id)
    }

    Menu {
        id: showThemeMenu
        objectName: "showThemeMenu"
        MenuItem {
            objectName: "showFollowShowItem"
            text: "\u2014 follow show default \u2014"
            onTriggered: docs.applyThemeToShow(playlistPanel.contextShowId, "")
        }
        Repeater {
            model: docs.themes
            MenuItem {
                required property var modelData
                objectName: "showApplyThemeItem"
                text: modelData.name
                onTriggered: docs.applyThemeToShow(playlistPanel.contextShowId,
                                                   modelData.id)
            }
        }
        MenuSeparator { objectName: "showMenuSeparator" }
        MenuItem {
            objectName: "showRemoveAllItem"
            text: "Remove all customizations in show"
            onTriggered: docs.removeShowCustomizationsFor(playlistPanel.contextShowId)
        }
    }

    Item {
        anchors.fill: parent

        LibraryBrowser {
            id: libraryBrowser
            objectName: "libraryBrowser"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: Math.max(120, Math.round(parent.height * 0.47))
            editingEnabled: playlistPanel.editingEnabled
            onImportRequested: playlistPanel.importRequested()
            onSongChosen: songId => playlistPanel.songChosen(songId)
            onThemeMenuRequested: (showId, x, y) => {
                playlistPanel.contextShowId = showId
                showThemeMenu.popup(x, y)
            }
        }

        CurrentServicePanel {
            id: servicePanel
            objectName: "currentServicePanel"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: libraryBrowser.bottom
            anchors.bottom: serviceDirText.top
            anchors.topMargin: 6
            editingEnabled: playlistPanel.editingEnabled
            onEntryChosen: showId => playlistPanel.sourceChosen("show", showId)
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
