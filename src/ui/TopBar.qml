import QtQuick
import QtQuick.Controls

// The toolbar (UI.6) is the plan's header row: Search, then the two
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
    signal searchRequested()
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
        ToolTab {
            objectName: "toolbarSearchButton"
            Accessible.name: "Search presentations (Ctrl+F)"
            glyph: "\u2315"
            label: "Search"
            // Exactly Ctrl+F: the sidebar owns the field and takes the
            // detail view to the Library first; the click itself selects
            // and presents nothing.
            onClicked: bar.searchRequested()
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
    }

    Row {
        anchors.right: parent.right
        anchors.rightMargin: 16
        anchors.verticalCenter: parent.verticalCenter
        spacing: 10
        Text {
            anchors.verticalCenter: parent.verticalCenter
            color: "#d8c07a"; font.pixelSize: 11
            visible: docs.dirty
            text: "Unsaved changes"
        }
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: 10; height: 10; radius: 5
            color: worker.outputLive ? "#63bd4c" : "#d46767"
        }
        Column {
            spacing: 3
            Text { color: "#eeeeee"; font.pixelSize: 12; text: "AUDIENCE" }
            Text {
                width: Math.min(260, bar.width * 0.25)
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
        // Settings is a mode, and the plan's header puts it at the far
        // right beside the audience state rather than among the operator
        // pages: Show and Edit run the service, this configures it.
        ToolTab {
            objectName: "toolbarTabSettings"
            Accessible.name: "Settings"
            glyph: "\u2699"
            label: "Settings"
            selected: bar.page === "settings"
            onClicked: bar.pageSelected("settings")
        }
    }
}
