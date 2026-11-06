import QtQuick
import QtQuick.Controls

// The toolbar (UI.6) is the plan's header row: the two
// operator pages, then media-bin visibility on the left; the audience
// state and Settings on the right. Every control here works on every
// page — none of them hide when a different page is selected, none of
// them are hover-only — and the two state-reading controls (the bin
// toggle, the audience dot and sentence) mirror their owners rather
// than keeping opinions: the bin says whether it is open, the worker
// says whether the audience has an output.
Rectangle {
    id: bar
    height: 64
    color: "#505052"
    property string page: "show"
    // Media-bin visibility: the state belongs to the panel store (the
    // bar's own strip and Ctrl+M toggle the same value), so this button
    // reflects it and asks for the change — never remembers its own.
    property bool drawerOpen: true
    signal pageSelected(string name)
    signal drawerToggleRequested()

    // One shape for everything in the strip: glyph over label, selected
    // pages carry the blue underline, the checkable visibility toggle
    // lights while it is on.
    component ToolTab: AbstractButton {
        id: tab
        property string glyph: ""
        property string label: ""
        property bool selected: false
        width: 66; height: 52
        padding: 0
        focusPolicy: Qt.NoFocus
        Accessible.name: label
        background: Rectangle {
            radius: 4
            color: tab.selected ? "#626265"
                 : tab.checked ? "#5b5b5e"
                 : tab.hovered ? "#59595b" : "transparent"
            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width; height: 2
                color: "#1685f8"
                visible: tab.selected
            }
        }
        contentItem: Item {
            Column {
                anchors.centerIn: parent
                width: parent.width
                spacing: 2
                Text {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: tab.glyph
                    color: tab.checked && !tab.selected ? "#8ec9f5" : "#dedede"
                    font.pixelSize: 23
                }
                Text {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: tab.label
                    color: tab.checked && !tab.selected ? "#8ec9f5" : "#dedede"
                    font.pixelSize: 12
                }
            }
        }
    }

    Row {
        id: navigation
        objectName: "toolbarNavigation"
        anchors.left: parent.left
        anchors.leftMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        spacing: 8
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: "OpenShowQt"
            color: "#eeeeee"
            font.pixelSize: 16
            font.bold: true
        }
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: 1; height: 32; color: "#666668"
        }
        Repeater {
            model: [{name: "show", label: "Show", glyph: "\u25b6"},
                    {name: "edit", label: "Edit", glyph: "\u270e"}]
            delegate: ToolTab {
                id: pageTab
                required property var modelData
                objectName: "toolbarTab"
                            + modelData.name.charAt(0).toUpperCase()
                            + modelData.name.slice(1)
                Accessible.name: modelData.label
                glyph: modelData.glyph
                label: modelData.label
                selected: bar.page === modelData.name
                onClicked: bar.pageSelected(modelData.name)
            }
        }
        ToolTab {
            objectName: "toolbarBinToggle"
            Accessible.name: bar.drawerOpen ? "Hide the media bin" : "Show the media bin"
            glyph: "\u25a4"
            label: "Media"
            checkable: true
            checked: bar.drawerOpen
            onClicked: bar.drawerToggleRequested()
        }
        // The themes entry (task TH.4, themes plan): ProPresenter's home for
        // themes is a header submenu, so the current theme is ONE click from
        // every page — the list with the worn one checked (clicking it takes
        // it off), and below a separator the Theme Editor mode. The menu
        // reads docs only: opening it sets and presents nothing.
        ToolTab {
            id: moreTab
            objectName: "toolbarMore"
            Accessible.name: "More"
            glyph: "\u22ef"
            label: "More"
            selected: bar.page === "themes" || bar.page === "bible" || bar.page === "songimport"
            Menu {
                id: themeMenu
                objectName: "themeMenu"
                Repeater {
                    model: docs.themes
                    MenuItem {
                        required property var modelData
                        objectName: "themeMenuItem"
                        text: (docs.wornThemeId === modelData.id ? "\u2713 " : "")
                              + modelData.name
                        // Worn check: clicking the checked row takes it off
                        // (the same set-or-clear the editor header offers).
                        onTriggered: docs.useThemeAsCurrent(
                                    docs.wornThemeId === modelData.id ? "" : modelData.id)
                    }
                }
                MenuItem {
                    objectName: "themeMenuEmpty"
                    visible: docs.themes.length === 0
                    enabled: false
                    text: "No themes yet"
                }
                MenuSeparator {}
                MenuItem {
                    objectName: "themeMenuEditor"
                    text: "Theme Editor\u2026"
                    onTriggered: bar.pageSelected("themes")
                }
                MenuSeparator {}
                MenuItem {
                    objectName: "songImportMenuItem"
                    text: "Import Song from Clipboard…"
                    onTriggered: bar.pageSelected("songimport")
                }
                // The Bible import (task BI.5): one requested passage at a
                // time into today's schedule. Opening the entry opens the
                // page; it asks for nothing and presents nothing until the
                // operator looks something up and presses Add.
                MenuItem {
                    objectName: "bibleImportMenuItem"
                    text: "Import Bible Passage\u2026"
                    onTriggered: bar.pageSelected("bible")
                }
            }
            onClicked: themeMenu.popup()
        }
    }

    Row {
        id: outputStateRow
        objectName: "toolbarOutputState"
        anchors.right: parent.right
        anchors.rightMargin: 16
        anchors.verticalCenter: parent.verticalCenter
        spacing: 10
        Text {
            id: dirtyLabel
            anchors.verticalCenter: parent.verticalCenter
            color: "#d8c07a"; font.pixelSize: 11
            visible: docs.dirty
            text: "Unsaved changes"
        }
        Rectangle {
            id: outputDot
            anchors.verticalCenter: parent.verticalCenter
            width: 10; height: 10; radius: 5
            color: worker.outputLive ? "#63bd4c" : "#d46767"
        }
        Column {
            // Reserve navigation and fixed actions first; only the status
            // sentence elides when Themes also needs its emergency Clear.
            width: Math.max(0, Math.min(260,
                bar.width - navigation.x - navigation.width - 12 - 16
                - (dirtyLabel.visible ? dirtyLabel.width + outputStateRow.spacing : 0)
                - outputDot.width - outputStateRow.spacing
                - (audienceClear.visible ? audienceClear.width + outputStateRow.spacing : 0)
                - settingsTab.width - outputStateRow.spacing))
            spacing: 3
            Text { width: parent.width; elide: Text.ElideRight; color: "#eeeeee"; font.pixelSize: 12; text: "AUDIENCE" }
            Text {
                width: parent.width
                elide: Text.ElideRight
                color: worker.statusNeedsAttention || worker.mediaError !== "" || worker.mediaAudio === "degraded"
                       || worker.mediaAudio === "no-output" ? "#e0a030" : "#cccccc"
                font.pixelSize: 11
                text: worker.mediaError !== "" || worker.standaloneError !== ""
                      || worker.foregroundErrors.length > 0 || worker.mediaAudio === "degraded"
                      || worker.mediaAudio === "no-output" ? "Media needs attention"
                    : worker.statusNeedsAttention ? "Action needs attention"
                    : !worker.outputLive ? "Output unavailable"
                    : worker.previewActive ? "Ready" : "Starting output"
                ToolTip.visible: statusHover.containsMouse
                ToolTip.text: "Connection and media details are available in Settings"
                MouseArea { id: statusHover; anchors.fill: parent; hoverEnabled: true; acceptedButtons: Qt.NoButton }
            }
        }
        // Themes hides the operator docks (task TV.2) — and with them the
        // Output panel's clear buttons. The emergency verb stays with the
        // audience state it acts on: the same Clear-all/Restore toggle the
        // panel offers (3.4), and the only control in this bar that shows
        // for one mode, because the only page that needs it is the one
        // with nowhere else to reach it.
        ToolTab {
            id: audienceClear
            objectName: "toolbarAudienceClear"
            Accessible.name: worker.canRestore ? "Restore the audience output"
                                               : "Clear the audience output"
            glyph: "\u2715"
            label: worker.canRestore ? "Restore" : "Clear"
            visible: bar.page === "themes"
            onClicked: worker.canRestore ? worker.restoreOutput() : worker.clearAll()
        }
        // Settings is a mode, and the plan's header puts it at the far
        // right beside the audience state rather than among the operator
        // pages: Show and Edit run the service, this configures it.
        ToolTab {
            id: settingsTab
            objectName: "toolbarTabSettings"
            Accessible.name: "Settings"
            glyph: "\u2699"
            label: "Settings"
            selected: bar.page === "settings"
            onClicked: bar.pageSelected("settings")
        }
    }
}
