import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material

/*
 * The operator workspace (M4 task 4.1): the reference's shell — a 40px
 * top bar over a row of left panel / center / right panel — with the
 * lyric flow built through M3 living inside it. The side panels are the
 * reference's resizable ones (SidePanel.qml + WorkspacePanels' measured
 * snap rules); the centre carries the slide list and the editor, and
 * the right panel is always the preview with the output tools below it,
 * like the reference's Preview + ShowTools.
 *
 * Theme tokens are the reference palette (docs/reference.md §3).
 *
 * Since task 6.19b3 the window's whole key table lives in ShortcutMap.qml;
 * what remains here is execution — the functions and buttons that perform
 * what a key asked, and the focus facts only the window can speak for.
 */
Window {
    id: root
    width: uiSize.width
    height: uiSize.height
    minimumWidth: 400   // the reference's window minimum
    minimumHeight: 400
    visible: true
    color: "#191923"  // --primary-darker, the canvas behind everything
    title: "OpenShowQt " + applicationVersion
    // Task 4.5: the F11/settings fullscreen is a persisted app setting,
    // so a projector setup survives restarts.
    visibility: appSettings.fullscreen ? Window.FullScreen : Window.Windowed

    Material.theme: Material.Dark
    Material.accent: "#f0008c"  // --secondary

    // Task 1.2: closing the UI shuts the worker down before the window
    // actually goes away.
    onClosing: function(close) {
        worker.shutdownNow()
    }

    // The workspace page under the top bar (4.3): "show" is the running
    // order and lyric editor, "edit" the text-box canvas. The remaining
    // reference pages arrive later and their buttons stay disabled.
    property string page: "show"

    // True while a text field holds the focus — the plain-arrow nudges
    // must not steal keystrokes from someone typing.
    readonly property bool typingIntoField: activeFocusItem !== null
        && (activeFocusItem instanceof TextField || activeFocusItem instanceof TextArea
            || activeFocusItem instanceof TextInput)

    // Panel building blocks (task 4.4): Material's default TextField is
    // too tall for a 250px column once the background is replaced (the
    // placeholder and text split vertically), and its Button is too wide
    // for three in a row. Since task 6.19b4 they are module files
    // (PanelField.qml, PanelButton.qml) carrying the workspace palette and
    // metrics, so the panels that got their own files style themselves
    // without borrowing the window's private declarations.

    function presentCurrentSlide() {
        const json = docs.currentSceneJson()
        if (json !== "") {
            worker.presentSceneJson(json)
        }
    }

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

    // Task 4.5: the presenter's steps walk the same navigation a Present
    // click armed, so a service runs from the keyboard alone.
    function presenterStep(delta) {
        if (docs.slides.length === 0) {
            return
        }
        docs.stepSlide(delta)
        root.presentCurrentSlide()
    }
    function presenterJump(index) {
        if (index >= 0 && index < docs.slides.length) {
            docs.selectSlide(index)
            root.presentCurrentSlide()
        }
    }

    // Task 6.19b3: the whole key table. The facts only the window and its
    // panels can speak for arrive as the properties below — which page is
    // up, what holds the focus (typing, a squatted button, the drawer, the
    // canvas's handles) and the on-air state the clear keys gate on — and
    // every activated key comes back as a signal handled here, beside the
    // same buttons that perform it. No key changed owner, sequence or gate
    // on the way out; two enabled owners for one key make Qt drop the key
    // entirely, which is why the gates moved WITH the shortcuts rather
    // than staying behind.
    ShortcutMap {
        page: root.page
        typingIntoField: root.typingIntoField
        squattedFocus: root.squattedFocus
        drawerFocused: libraryDrawer.containsFocus
        boxSelected: editPage.boxSelected
        currentBox: docs.currentBox
        slideCount: docs.slides.length
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

    /* ------------------------------------------------------------------
     * Left panel — the show page of the reference: playlist chips over
     * the running order. Task 6.19b5: the dock is PlaylistPanel.qml —
     * its chips, rows, reorder handles and drop target all address
     * `docs` and the rows are its projections; what the window owns is
     * where the dock sits and the id the centre pages anchor to.
     * ------------------------------------------------------------------ */
    PlaylistPanel {
        id: leftDock
        anchors.left: parent.left
        anchors.top: topBar.bottom
        anchors.bottom: libraryDrawer.top
    }

    /* ------------------------------------------------------------------
     * Right panel — always the preview, with the show's output tools
     * below it (the reference's Preview + ShowTools arrangement). Task
     * 6.19b4: the dock itself is OutputPanel.qml — it mirrors `worker`
     * status and drags through `panels` on its own; what the window still
     * owns is where it sits, between the top bar and the drawer, and the
     * id the two centre pages anchor their right edges to.
     * ------------------------------------------------------------------ */
    OutputPanel {
        id: rightDock
        anchors.right: parent.right
        anchors.top: topBar.bottom
        anchors.bottom: libraryDrawer.top
    }

    /* ------------------------------------------------------------------
     * Center — two pages (4.3) on the reference's show/edit split. Task
     * 6.19b6: the show page is ShowPage.qml — the slide list with the
     * worker's thumbnails, follow-presenting, the history buttons and the
     * lyric editor. What the window owns is placement, which page is up,
     * and presenting itself: presentCurrentSlide() is also what the
     * keyboard executors run, so the page asks and this performs.
     * ------------------------------------------------------------------ */
    ShowPage {
        visible: root.page === "show"
        anchors.left: leftDock.right
        anchors.right: rightDock.left
        anchors.top: topBar.bottom
        anchors.bottom: libraryDrawer.top
        anchors.margins: 12

        onPresentRequested: root.presentCurrentSlide()
    }

    /* ------------------------------------------------------------------
     * The edit page (task 4.3, stack since 6.17a2). Task 6.19b7: the page
     * is EditPage.qml — the 16:9 stage over EditCanvas with the worker's
     * picture of the slide under edit, the exact geometry fields, the
     * stack commands and the style panel. What the window owns is
     * placement, which page is up, and the key map's share: the
     * selection flag its two Escape owners hand off on, and the
     * nudge/step executors the page performs on its own canvas.
     * ------------------------------------------------------------------ */
    EditPage {
        id: editPage
        visible: root.page === "edit"
        anchors.left: leftDock.right
        anchors.right: rightDock.left
        anchors.top: topBar.bottom
        anchors.bottom: libraryDrawer.top
        anchors.margins: 12
    }

    /* ------------------------------------------------------------------
     * The settings page (task 4.5): the essential settings, persisted
     * through AppSettings — presenter number keys and the F11
     * fullscreen — plus a cheat sheet of the shortcuts the keyboard
     * flows rely on, and the service information rows. The reference
     * keeps its shortcuts hardcoded and undocumented; documenting them
     * in-app is what makes "core flows work by keyboard" learnable.
     * ------------------------------------------------------------------ */
    Item {
        id: settingsPage
        visible: root.page === "settings"
        anchors.left: leftDock.right
        anchors.right: rightDock.left
        anchors.top: topBar.bottom
        anchors.bottom: libraryDrawer.top
        anchors.margins: 12

        Column {
            width: Math.min(620, parent.width)
            spacing: 5

            Text {
                color: "#f0f0ff"
                font.pixelSize: 15
                font.bold: true
                text: "Settings"
            }

            Text {
                color: "#8f93a5"
                font.pixelSize: 10
                topPadding: 10
                text: "PRESENTER"
            }
            CheckBox {
                text: "Number keys play slides (1\u20139, 0 = ten)"
                font.pixelSize: 13
                checked: appSettings.numberKeysPresent
                onToggled: appSettings.numberKeysPresent = checked
            }
            Text {
                color: "#8f93a5"
                font.pixelSize: 11
                wrapMode: Text.WordWrap
                width: parent.width
                text: "Presenter keys always work: Space, PageDown, PageUp, arrows, Home and End walk the current show straight onto the output."
            }

            Text {
                color: "#8f93a5"
                font.pixelSize: 10
                topPadding: 10
                text: "WINDOW"
            }
            CheckBox {
                text: "Fullscreen (F11)"
                font.pixelSize: 13
                checked: appSettings.fullscreen
                onToggled: appSettings.fullscreen = checked
            }

            Text {
                color: "#8f93a5"
                font.pixelSize: 10
                topPadding: 10
                text: "SHORTCUTS"
            }
            Repeater {
                model: [
                    { key: "Space / PageDown / Right / F5",
                      what: "Next slide \u2014 straight to the output" },
                    { key: "PageUp / Left", what: "Previous slide" },
                    { key: "Home / End", what: "First / last slide" },
                    { key: ". ", what: "Clear everything on air" },
                    { key: "Esc", what: "Clear all \u2014 or restore; deselects first on the edit page" },
                    { key: "F1 / F2", what: "Clear the background / the slide" },
                    { key: "Ctrl+1 / Ctrl+2 / Ctrl+5", what: "Show, edit and settings pages" },
                    { key: "Ctrl+F", what: "Library search" },
                    { key: "Ctrl+D", what: "Duplicate the slide \u2014 opens the drawer when there is none" },
                    { key: "Ctrl+C / Ctrl+X / Ctrl+V", what: "Copy / cut / paste the slide \u2014 text fields keep theirs" },
                    { key: "Ctrl+Z \u00b7 Ctrl+Y / Ctrl+Shift+Z", what: "Undo \u00b7 redo" },
                    { key: "Ctrl+S", what: "Save the service file" },
                    { key: "Arrows / Ctrl+Arrows", what: "Nudge the selected box 1px / 10px (edit page)" },
                    { key: "Tab / Shift+Tab", what: "Select the next / previous text box (edit page)" },
                    { key: "F11", what: "Fullscreen" }
                ]
                delegate: Row {
                    required property var modelData
                    spacing: 14
                    Text {
                        width: 250
                        color: "#f0f0ff"
                        font.pixelSize: 12
                        text: modelData.key
                    }
                    Text {
                        color: "#8f93a5"
                        font.pixelSize: 12
                        text: modelData.what
                    }
                }
            }

            Text {
                color: "#8f93a5"
                font.pixelSize: 10
                topPadding: 10
                text: "SERVICE"
            }
            Row {
                spacing: 14
                Text {
                    width: 250
                    color: "#f0f0ff"
                    font.pixelSize: 12
                    text: "Version"
                }
                Text {
                    color: "#8f93a5"
                    font.pixelSize: 12
                    text: applicationVersion + " \u00b7 worker " + worker.status
                }
            }
            Row {
                spacing: 14
                Text {
                    width: 250
                    color: "#f0f0ff"
                    font.pixelSize: 12
                    text: "Service directory"
                }
                Text {
                    color: "#8f93a5"
                    font.pixelSize: 12
                    text: docs.serviceDirectory
                }
            }
            Row {
                spacing: 14
                Text {
                    width: 250
                    color: "#f0f0ff"
                    font.pixelSize: 12
                    text: "Recovery autosave"
                }
                Text {
                    color: "#8f93a5"
                    font.pixelSize: 12
                    text: "saved with every edit \u00b7 Ctrl+S writes the service file"
                }
            }
        }
    }

    /* ------------------------------------------------------------------
     * Bottom library drawer (task 4.2): the searchable Shows/Media strip.
     * Its height is WorkspacePanels' (300 open, 40 closed). The type is
     * named LibraryDrawer so it doesn't shadow QtQuick.Controls' Drawer.
     * ------------------------------------------------------------------ */
    LibraryDrawer {
        id: libraryDrawer
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
    }
}
