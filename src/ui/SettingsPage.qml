import QtQuick
import QtQuick.Controls

/*
 * The settings page (task 4.5; task 6.19b8 out of Main.qml): the essential
 * settings, persisted through AppSettings — presenter number keys and the
 * F11 fullscreen — plus a cheat sheet of the shortcuts the keyboard flows
 * rely on, and the service information rows. The reference keeps its
 * shortcuts hardcoded and undocumented; documenting them in-app is what
 * makes "core flows work by keyboard" learnable.
 *
 * The cheat sheet below is the operator-facing mirror of the key table in
 * ShortcutMap.qml — the two must be read side by side, and every entry
 * here has exactly the owners and sequences that file defines. It came
 * across verbatim with the move, and was checked entry by entry against
 * the map: presenter walking, the "." clear, the Escape hand-off that
 * deselects first on the edit page, the Ctrl+D duplicate-or-drawer rule,
 * the clipboard letters that text fields keep, both redo spellings, the
 * nudges, the Tab walk and F11. If a key changes owner, sequence or gate,
 * ShortcutMap.qml is where it happens and this list is the promise the
 * operator was given — change both.
 *
 * Nothing here belongs to the window: the toggles read and write the
 * persisted `appSettings`, the version line the `applicationVersion` and
 * the worker's status, the service rows the controller's directory — all
 * context properties. So the component takes no properties and emits no
 * signals, the same honest finding the two docks and the show page
 * reached before it.
 */
Item {
    id: settingsPage

    Column {
        width: Math.min(620, parent.width)
        spacing: 5

        Text {
            color: "#eeeeee"
            font.pixelSize: 15
            font.bold: true
            text: "Settings"
        }

        Text {
            color: "#b0b0b0"
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
            color: "#b0b0b0"
            font.pixelSize: 11
            wrapMode: Text.WordWrap
            width: parent.width
            text: "Presenter keys always work: Space, PageDown, PageUp, arrows, Home and End walk the current show straight onto the output."
        }

        Text {
            color: "#b0b0b0"
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
            color: "#b0b0b0"
            font.pixelSize: 10
            topPadding: 10
            text: "SHORTCUTS"
        }
        Repeater {
            model: [
                { key: "Space / PageDown / Right / F5",
                  what: "Next slide in Show \u2014 straight to the output" },
                { key: "PageUp / Left", what: "Previous slide" },
                { key: "Home / End", what: "First / last slide" },
                { key: ". ", what: "Clear everything on air" },
                { key: "Esc", what: "Clear all \u2014 or restore; deselects first on the edit page" },
                { key: "F1 / F2", what: "Clear the background / the slide" },
                { key: "Ctrl+1 / Ctrl+2 / Ctrl+5", what: "Show, edit and settings pages" },
                { key: "Ctrl+F", what: "Library search" },
                { key: "Ctrl+D", what: "Duplicate in Edit; toggle the library in Show" },
                { key: "Ctrl+C / Ctrl+X / Ctrl+V", what: "Copy slide; cut / paste in Edit only" },
                { key: "Ctrl+Z \u00b7 Ctrl+Y / Ctrl+Shift+Z", what: "Undo \u00b7 redo (Edit only)" },
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
                    color: "#eeeeee"
                    font.pixelSize: 12
                    text: modelData.key
                }
                Text {
                    color: "#b0b0b0"
                    font.pixelSize: 12
                    text: modelData.what
                }
            }
        }

        Text {
            color: "#b0b0b0"
            font.pixelSize: 10
            topPadding: 10
            text: "SERVICE"
        }
        Row {
            spacing: 14
            Text {
                width: 250
                color: "#eeeeee"
                font.pixelSize: 12
                text: "Version"
            }
            Text {
                color: "#b0b0b0"
                font.pixelSize: 12
                text: applicationVersion + " \u00b7 worker " + worker.status
            }
        }
        Row {
            spacing: 14
            Text {
                width: 250
                color: "#eeeeee"
                font.pixelSize: 12
                text: "Service directory"
            }
            Text {
                color: "#b0b0b0"
                font.pixelSize: 12
                text: docs.serviceDirectory
            }
        }
        Row {
            spacing: 14
            Text {
                width: 250
                color: "#eeeeee"
                font.pixelSize: 12
                text: "Recovery autosave"
            }
            Text {
                color: "#b0b0b0"
                font.pixelSize: 12
                text: "saved with every edit \u00b7 Ctrl+S writes the service file"
            }
        }
    }
}
