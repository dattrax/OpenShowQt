import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material

// The operator shell keeps the running order, slide grid and audience
// monitor visible together. Content authoring belongs to the Edit page.
Window {
    id: root
    width: uiSize.width
    height: uiSize.height
    minimumWidth: 1000
    minimumHeight: 680
    visible: true
    color: "#202020"  // --primary-darker, the canvas behind everything
    title: "OpenShowQt " + applicationVersion
    // Task 4.5: the F11/settings fullscreen is a persisted app setting,
    // so a projector setup survives restarts.
    visibility: appSettings.fullscreen ? Window.FullScreen : Window.Windowed

    Material.theme: Material.Dark
    Material.accent: "#1685f8"
    Material.primary: "#505050"
    Material.background: "#353535"
    Material.foreground: "#eeeeee"

    // Task 1.2: closing the UI shuts the worker down before the window
    // actually goes away.
    onClosing: function(close) {
        worker.shutdownNow()
    }

    // Explicit modes separate presenting from content authoring.
    property string page: "show"
    // Switching modes has to hand the keyboard over. Qt does not take the
    // active focus from an item whose page is only hidden, so the lyric box
    // the operator was typing in would go on owning every keystroke — the
    // presenter's included — from a page nobody can see (UI.3c found it:
    // Page Up/Down dead after Edit). Release whatever the leaving page held,
    // then let the window take focus back so the presenter gates are live.
    onPageChanged: {
        const held = activeFocusItem
        if (held) held.focus = false
        contentItem.forceActiveFocus()
    }

    // True while a text field holds the focus — the plain-arrow nudges
    // must not steal keystrokes from someone typing.
    readonly property bool typingIntoField: activeFocusItem !== null
        && (activeFocusItem instanceof TextField || activeFocusItem instanceof TextArea
            || activeFocusItem instanceof TextInput)

    // What the middle of the screen shows (UI.3b): "" follows the running
    // order under preparation; a presentation browsed from the sidebar shows
    // its slides off air. Browsing selects, it never presents.
    property string browsedShowId: ""

    // A focused button or combo eats Space and the arrow keys themselves;
    // the presenter gates stand down for it (task 4.5, AbstractButton is
    // both in the reference's rule). The window has to be the one to say
    // this: activeFocusItem is the window's, and a panel's own
    // activeFocusItem would answer a different question.
    readonly property bool squattedFocus: activeFocusItem !== null
        && (activeFocusItem instanceof AbstractButton || activeFocusItem instanceof ComboBox)

    // The key actions themselves (task 6.19b3) live in ShortcutMap.qml —
    // every sequence, its gate, and the intent it carries, so R17's
    // one-owner-per-key rule reads in one file. What stays here is
    // execution: the map emits an intent, and these functions (and the
    // buttons below) perform it against the controller and the worker.
    // The two canvas executors the map also drives — the nudge and the
    // stack step — are the edit page's, since only its EditCanvas tracks
    // the live rectangle (task 6.19b7).

    // Task 4.5 (rewritten by UI.3b): the presenter's keys walk the CHOSEN
    // running order — across presentation boundaries and repeated entries —
    // and every step is a go. The editing cursor stays where it was.
    function presenterStep(delta) {
        showPage.stepCue(delta)
    }
    function presenterJump(index) {
        showPage.jumpToCue(index)
    }

    // One shortcut owner for each key, with authoring gated to Edit.
    ShortcutMap {
        page: root.page
        typingIntoField: root.typingIntoField
        squattedFocus: root.squattedFocus
        drawerFocused: libraryDrawer.containsFocus
        boxSelected: editPage.boxSelected
        currentBox: docs.currentBox
        slideCount: docs.slides.length
        cueCount: showPage.cueRows.length
        wordsOnAir: worker.wordsOnAir
        backgroundOnAir: worker.backgroundOnAir
        canRestore: worker.canRestore
        numberKeysPresent: appSettings.numberKeysPresent

        // Escape clears all — and restores when the output is already
        // clear, matching the button's Clear-all/Restore toggle (3.4).
        onOutputCancelRequested: worker.canRestore ? worker.restoreOutput()
                                                   : worker.clearAll()
        onDeselectBoxRequested: editPage.boxSelected = false
        onClearBackgroundRequested: worker.clearBackground()
        onClearWordsRequested: worker.clearSlide()
        onDuplicateSlideRequested: docs.duplicateCurrentSlide()
        onToggleDrawerRequested: panels.toggleDrawer()
        onFocusDrawerSearchRequested: libraryDrawer.focusSearch()
        onPageSelected: name => root.page = name
        onBoxNudgeRequested: (dx, dy) => editPage.nudgeBox(dx, dy)
        onBoxStepRequested: delta => editPage.stepBox(delta)
        onPresenterStepRequested: delta => root.presenterStep(delta)
        onPresenterJumpRequested: index => root.presenterJump(index)
        onClearAllRequested: worker.clearAll()
        onUndoRequested: docs.undo()
        onRedoRequested: docs.redo()
        onSaveRequested: docs.save()
        onCopyRequested: docs.copyCurrentSlide()
        onCutRequested: docs.cutCurrentSlide()
        onPasteRequested: docs.pasteSlide()
        onFullscreenToggled: appSettings.fullscreen = !appSettings.fullscreen
    }

    TopBar {
        id: topBar
        page: root.page
        onPageSelected: name => root.page = name
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
    }

    PlaylistPanel {
        id: leftDock
        objectName: "libraryDock"
        editingEnabled: root.page === "edit"
        width: panels.leftWidth <= 8 ? panels.leftWidth
               : Math.min(panels.leftWidth, root.width * 0.24)
        anchors.left: parent.left
        anchors.top: topBar.bottom
        anchors.bottom: libraryDrawer.top
        onSourceChosen: (kind, id) => root.browsedShowId = kind === "show" ? id : ""
    }

    OutputPanel {
        id: rightDock
        objectName: "outputDock"
        width: panels.rightWidth <= 8 ? panels.rightWidth
               : Math.min(Math.max(320, panels.rightWidth), root.width * 0.34)
        anchors.right: parent.right
        anchors.top: topBar.bottom
        anchors.bottom: parent.bottom
    }

    ShowPage {
        id: showPage
        objectName: "showPage"
        visible: root.page === "show"
        anchors.left: leftDock.right
        anchors.right: rightDock.left
        anchors.top: topBar.bottom
        anchors.bottom: libraryDrawer.top
        anchors.margins: 0

        sourceShowId: root.browsedShowId
        onEditRequested: root.page = "edit"
    }

    EditPage {
        id: editPage
        visible: root.page === "edit"
        anchors.left: leftDock.right
        anchors.right: rightDock.left
        anchors.top: topBar.bottom
        anchors.bottom: libraryDrawer.top
        anchors.margins: 12
    }

    SettingsPage {
        visible: root.page === "settings"
        anchors.left: leftDock.right
        anchors.right: rightDock.left
        anchors.top: topBar.bottom
        anchors.bottom: libraryDrawer.top
        anchors.margins: 12
    }

    LibraryDrawer {
        id: libraryDrawer
        objectName: "libraryDrawer"
        editingEnabled: root.page === "edit"
        height: Math.min(panels.drawerHeight, Math.max(40,
                    root.height - topBar.height - 300))
        anchors.left: parent.left
        anchors.right: rightDock.left
        anchors.bottom: parent.bottom
    }
}
