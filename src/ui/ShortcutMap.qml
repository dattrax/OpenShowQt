import QtQuick

/*
 * The window's whole key table (task 6.19b3, from Main.qml keys earned in
 * 3.4, 4.2, 4.3, 4.5 and 6.17a2) in one component: every sequence the
 * operator can press, the gate that decides who may answer it, and the
 * intent it carries — emitted as a signal for the window to execute.
 *
 * Why the gate lives here and the action lives there: a key's MEANING is
 * "when nothing else may have this key, do THIS", and the "nothing else"
 * half — typing keeps its keystrokes, the drawer keeps its arrows, a box
 * with the handles keeps Left/Right, a focused button keeps Space — is only
 * readable when every claimant is on the same page. Qt drops a key that two
 * enabled Shortcuts claim at once, so these gates are load-bearing in a way
 * ordinary bindings are not, and R17's one-owner-per-key rule is what makes
 * this file, not a code review across two thousand lines, the place the rule
 * is checked. What stays in the window is execution: the signals land beside
 * the same buttons that perform the actions, and the facts only the window
 * can speak for (which item holds the focus, which page is up, the drawer's
 * focus, the canvas's selection) arrive as the properties below.
 *
 * The keys, block by block:
 *
 *   Escape / F1 / F2          output clear, clear-background, clear-words (3.4)
 *   Ctrl+D / Ctrl+F           duplicate-or-drawer, drawer search (4.2)
 *   Ctrl+1..5                 pages (4.3)
 *   arrows, Ctrl+arrows       nudge the selected box 1px / 10px (4.3)
 *   Tab, Shift+Tab            step the selection through the stack (6.17a2)
 *   Space/PgDn/F5/PgUp/Home/End/Left/Right/"."  presenter keys (4.5)
 *   0..9                      presenter number keys, opt-in (4.5)
 *   Undo/Redo/Save/Ctrl+C/X/V the document keys (4.5)
 *   F11                       fullscreen, remembered (4.5)
 *
 * The operator-facing mirror of this table is the cheat sheet on the
 * settings page (SettingsPage.qml); a key changed here is a promise
 * changed there, and the two are read side by side.
 */
Item {
    id: shortcutMap

    // What the window owns and the gates read. Each is a fact, not a
    // decision: the decisions are the enabled: lines below.
    property string page: "show"
    property bool typingIntoField: false
    property bool squattedFocus: false      // a button or combo holds it
    property bool drawerFocused: false
    property bool boxSelected: false        // the canvas's handles are on
    property int currentBox: -1             // docs.currentBox
    property int slideCount: 0
    property bool wordsOnAir: false
    property bool backgroundOnAir: false
    property bool canRestore: false
    property bool numberKeysPresent: false  // the settings page's opt-in

    // What a pressed key asks the window to do.
    signal outputCancelRequested()
    signal deselectBoxRequested()
    signal clearBackgroundRequested()
    signal clearWordsRequested()
    signal duplicateSlideRequested()
    signal toggleDrawerRequested()
    signal focusDrawerSearchRequested()
    signal pageSelected(string name)
    signal boxNudgeRequested(real dx, real dy)
    signal boxStepRequested(int delta)
    signal presenterStepRequested(int delta)
    signal presenterJumpRequested(int index)
    signal clearAllRequested()
    signal undoRequested()
    signal redoRequested()
    signal saveRequested()
    signal copyRequested()
    signal cutRequested()
    signal pasteRequested()
    signal fullscreenToggled()

    // The shared gates. nudging and the presenter arrows are mutually
    // exclusive by construction (the arrow pair below adds !nudging), like
    // the two Escape owners at the top of the old Main.qml list.
    readonly property bool nudging: page === "edit" && boxSelected
                                    && !typingIntoField && slideCount > 0
    readonly property bool boxStepping: page === "edit" && currentBox >= 0
                                        && !typingIntoField
    readonly property bool presenterTyping: typingIntoField || drawerFocused
                                            || squattedFocus
    readonly property bool presenterNav: !presenterTyping && slideCount > 0
    readonly property bool numberKeys: numberKeysPresent && !presenterTyping
                                       && page !== "edit" && slideCount > 0

    // Task 3.4: the reference's presenter keys. Escape clears all — and
    // restores when the output is already clear, matching the button's
    // Clear-all/Restore toggle. Disabled controls (nothing on air) simply
    // do nothing. On the edit page the first Escape steps out of the
    // selection first (4.3); the two owners gate on boxSelected, so the
    // key never lands on both at once.
    Shortcut {
        sequence: StandardKey.Cancel
        enabled: (wordsOnAir || backgroundOnAir || canRestore)
                 && !(page === "edit" && boxSelected)
        onActivated: outputCancelRequested()
    }
    Shortcut {
        sequence: "Escape"
        enabled: page === "edit" && boxSelected
        onActivated: deselectBoxRequested()
    }
    Shortcut {
        sequence: "F1"
        enabled: backgroundOnAir
        onActivated: clearBackgroundRequested()
    }
    Shortcut {
        sequence: "F2"
        enabled: wordsOnAir
        onActivated: clearWordsRequested()
    }

    // Task 4.2: the drawer keys the reference carries (Ctrl+F opens its
    // search; Ctrl+D toggles the library strip unless a slide is current,
    // when it duplicates instead — the reference's selection rule, 4.5).
    // Ctrl+1..2 inside the drawer still pick its tabs — see LibraryDrawer
    // — but from the window they now pick pages, like the reference's
    // Ctrl+1..5 (4.3); 3..4 belong to pages that do not exist yet and do
    // nothing.
    Shortcut {
        sequence: "Ctrl+D"
        enabled: !typingIntoField && slideCount > 0
        onActivated: duplicateSlideRequested()
    }
    Shortcut {
        sequence: "Ctrl+D"
        enabled: !typingIntoField && slideCount === 0
        onActivated: toggleDrawerRequested()
    }
    Shortcut {
        sequence: "Ctrl+F"
        onActivated: focusDrawerSearchRequested()
    }
    // The window's Ctrl+1..5 pick pages like the reference; 3..5 belong to
    // pages that do not exist yet, so those stay switched off. While the
    // keyboard is inside the drawer, Ctrl+1/2 stay with its tabs instead.
    Shortcut {
        sequence: "Ctrl+1"
        enabled: !drawerFocused
        onActivated: pageSelected("show")
    }
    Shortcut {
        sequence: "Ctrl+2"
        enabled: !drawerFocused
        onActivated: pageSelected("edit")
    }
    Shortcut {
        sequence: "Ctrl+3"
        enabled: false  // stage page arrives later
    }
    Shortcut {
        sequence: "Ctrl+4"
        enabled: false  // draw page arrives later
    }
    Shortcut {
        sequence: "Ctrl+5"
        enabled: !drawerFocused
        onActivated: pageSelected("settings")
    }

    // Task 4.3: the reference's nudge rule (EditTools.svelte:314-326) —
    // arrows walk the selected text box 1px, Ctrl+arrows 10px. Every step
    // is one undo entry, so it only runs on the edit page with a box
    // selected and nobody typing. Since 6.17a2 "the selected box" is the
    // session's selection, not the slide's one tuple: the canvas mirrors
    // its rectangle, and arrows, drag, fields and inspector all move the
    // box that wears the handles.
    Shortcut {
        sequence: "Left"
        enabled: shortcutMap.nudging
        onActivated: boxNudgeRequested(-1, 0)
    }
    Shortcut {
        sequence: "Right"
        enabled: shortcutMap.nudging
        onActivated: boxNudgeRequested(1, 0)
    }
    Shortcut {
        sequence: "Up"
        enabled: shortcutMap.nudging
        onActivated: boxNudgeRequested(0, -1)
    }
    Shortcut {
        sequence: "Down"
        enabled: shortcutMap.nudging
        onActivated: boxNudgeRequested(0, 1)
    }
    Shortcut {
        sequence: "Ctrl+Left"
        enabled: shortcutMap.nudging
        onActivated: boxNudgeRequested(-10, 0)
    }
    Shortcut {
        sequence: "Ctrl+Right"
        enabled: shortcutMap.nudging
        onActivated: boxNudgeRequested(10, 0)
    }
    Shortcut {
        sequence: "Ctrl+Up"
        enabled: shortcutMap.nudging
        onActivated: boxNudgeRequested(0, -10)
    }
    Shortcut {
        sequence: "Ctrl+Down"
        enabled: shortcutMap.nudging
        onActivated: boxNudgeRequested(0, 10)
    }

    // Task 6.17a2: Tab steps the selection through the slide's stack and
    // Shift+Tab steps back, wrapping — the keyboard reaches every box the
    // canvas outlines, so a slide of eight does not demand eight mouse
    // presses. Same gates as the nudge: edit page, a box to select, and
    // nobody typing (in a field, Tab belongs to the field).
    Shortcut {
        sequence: "Tab"
        enabled: shortcutMap.boxStepping
        onActivated: boxStepRequested(1)
    }
    Shortcut {
        sequence: "Shift+Tab"
        enabled: shortcutMap.boxStepping
        onActivated: boxStepRequested(-1)
    }

    // Task 4.5: the reference's presenter keys (shortcuts.ts) — Space,
    // PageDown and Right advance, PageUp and Left step back, Home and End
    // jump, F5 advances for presenter clickers, "." clears all. They walk
    // the same navigation a Present click armed, so a service runs from
    // the keyboard alone. The gates are the reference's: typing keeps its
    // keys, a focused button or combo eats Space (AbstractButton is both),
    // the library drawer owns arrows for its own list, and the edit page's
    // selected box owns Left/Right — nudging and presenter arrows are
    // mutually exclusive, like the Escape pair.
    Shortcut {
        sequence: "Space"
        enabled: shortcutMap.presenterNav
        onActivated: presenterStepRequested(1)
    }
    Shortcut {
        sequence: "PageDown"
        enabled: shortcutMap.presenterNav
        onActivated: presenterStepRequested(1)
    }
    Shortcut {
        sequence: "F5"
        enabled: shortcutMap.presenterNav
        onActivated: presenterStepRequested(1)
    }
    Shortcut {
        sequence: "PageUp"
        enabled: shortcutMap.presenterNav
        onActivated: presenterStepRequested(-1)
    }
    Shortcut {
        sequence: "Left"
        enabled: shortcutMap.presenterNav && !shortcutMap.nudging
        onActivated: presenterStepRequested(-1)
    }
    Shortcut {
        sequence: "Right"
        enabled: shortcutMap.presenterNav && !shortcutMap.nudging
        onActivated: presenterStepRequested(1)
    }
    Shortcut {
        sequence: "Home"
        enabled: shortcutMap.presenterNav
        onActivated: presenterJumpRequested(0)
    }
    Shortcut {
        sequence: "End"
        enabled: shortcutMap.presenterNav
        onActivated: presenterJumpRequested(slideCount - 1)
    }
    Shortcut {
        sequence: "."
        enabled: shortcutMap.presenterNav && (wordsOnAir || backgroundOnAir)
        onActivated: clearAllRequested()
    }

    // Task 4.5: the core document keys. While a field has the focus the
    // editing keys belong to it — undo, redo and the clipboard letters
    // all have native text-field meanings — so they are gated away from
    // typing. Ctrl+S is not a keystroke any field wants, so saving works
    // at any time (it flushes the pending edit first, like every other
    // controller call). The clipboard is the controller's slide clipboard
    // — the system one stays with the text fields, exactly as in the
    // reference.
    Shortcut {
        sequence: StandardKey.Undo
        enabled: !typingIntoField
        onActivated: undoRequested()
    }
    Shortcut {
        sequence: "Ctrl+Y"
        enabled: !typingIntoField
        onActivated: redoRequested()
    }
    Shortcut {
        sequence: "Ctrl+Shift+Z"
        enabled: !typingIntoField
        onActivated: redoRequested()
    }
    Shortcut {
        sequence: StandardKey.Save
        onActivated: saveRequested()
    }
    // Explicit letters rather than StandardKey.Copy/Cut/Paste: the
    // standard keys also carry Ctrl+Ins-style bindings that this app has
    // no use for, and each one logs a binding warning at startup.
    Shortcut {
        sequence: "Ctrl+C"
        enabled: !typingIntoField
        onActivated: copyRequested()
    }
    Shortcut {
        sequence: "Ctrl+X"
        enabled: !typingIntoField
        onActivated: cutRequested()
    }
    Shortcut {
        sequence: "Ctrl+V"
        enabled: !typingIntoField
        onActivated: pasteRequested()
    }

    // Presenter number keys, the reference's opt-in: 1..9 play slide
    // one..nine and 0 plays ten. Off until the settings page enables it,
    // and silent while typing, inside the drawer, or on the edit page.
    Shortcut {
        sequence: "1"
        enabled: shortcutMap.numberKeys
        onActivated: presenterJumpRequested(0)
    }
    Shortcut {
        sequence: "2"
        enabled: shortcutMap.numberKeys
        onActivated: presenterJumpRequested(1)
    }
    Shortcut {
        sequence: "3"
        enabled: shortcutMap.numberKeys
        onActivated: presenterJumpRequested(2)
    }
    Shortcut {
        sequence: "4"
        enabled: shortcutMap.numberKeys
        onActivated: presenterJumpRequested(3)
    }
    Shortcut {
        sequence: "5"
        enabled: shortcutMap.numberKeys
        onActivated: presenterJumpRequested(4)
    }
    Shortcut {
        sequence: "6"
        enabled: shortcutMap.numberKeys
        onActivated: presenterJumpRequested(5)
    }
    Shortcut {
        sequence: "7"
        enabled: shortcutMap.numberKeys
        onActivated: presenterJumpRequested(6)
    }
    Shortcut {
        sequence: "8"
        enabled: shortcutMap.numberKeys
        onActivated: presenterJumpRequested(7)
    }
    Shortcut {
        sequence: "9"
        enabled: shortcutMap.numberKeys
        onActivated: presenterJumpRequested(8)
    }
    Shortcut {
        sequence: "0"
        enabled: shortcutMap.numberKeys
        onActivated: presenterJumpRequested(9)
    }

    // F11 flips the window between fullscreen and windowed (the
    // reference's app fullscreen) and is remembered, so a projector
    // setup survives restarts.
    Shortcut {
        sequence: "F11"
        onActivated: fullscreenToggled()
    }
}
