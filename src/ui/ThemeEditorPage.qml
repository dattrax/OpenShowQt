import QtQuick
import QtQuick.Controls

/*
 * The Theme Editor (task TH.4, themes plan): the theme list left (New,
 * Duplicate, double-click Rename, Delete), the inspector right (every
 * property of the selected theme), and centre a large live preview of a
 * built-in sample cue rendered in the selected theme through the worker's
 * own off-air lane (6.16a). The sample is a fixture, never a document:
 * clicking through the list asks the worker to draw each theme's picture of
 * the SAME words, and the audience output never moves. Only one page owns
 * that lane at a time because only one page is visible at a time — the same
 * contract the Edit page keeps (6.16a), including standing the target down
 * on the way out and showing an honest blank when no worker is connected.
 */
Item {
    id: themeEditor
    objectName: "themeEditorPage"

    // The theme under edit. "" means the service has none yet, and every
    // control that needs a target disables itself honestly on it.
    property string selectedId: ""
    // While a row is being renamed in place the list shows its field
    // instead of its label; one row at a time, owned here, so the commit
    // and the cancel each have exactly one home.
    property string renamingId: ""
    // The live record of the selection, re-read from the model's own list
    // so an edit (or a delete) repaints the inspector from truth.
    function rowFor(id) {
        if (id === "")
            return null
        for (const theme of docs.themes) {
            if (theme.id === id)
                return theme
        }
        return null
    }
    readonly property var row: rowFor(selectedId)

    function selectTheme(id) {
        if (id !== selectedId) {
            selectedId = id
            askForSamplePicture()
        }
    }
    // Re-pick the selection when the list moved under it — a delete, or
    // the first theme added to an empty service. Selection prefers the
    // worn theme: that is the one the operator sees through the show.
    function reconcileSelection() {
        const themes = docs.themes
        if (themes.some(t => t.id === selectedId))
            return
        if (docs.wornThemeId !== "" && themes.some(t => t.id === docs.wornThemeId))
            themeEditor.selectedId = docs.wornThemeId
        else
            themeEditor.selectedId = themes.length > 0 ? themes[0].id : ""
        syncInspector()
    }
    // The fields SHOW the selection's values by assignment, not binding:
    // a field the operator is typing in must not be overwritten mid-word,
    // and after a commit the model is re-read whole anyway.
    function syncInspector() {
        const row = themeEditor.row
        nameField.text = row !== null ? row.name : ""
        colourField.text = row !== null ? row.textColor : ""
        backField.text = row !== null ? row.backgroundColor : ""
        gradientField.text = row !== null ? row.backgroundGradient : ""
        fontField.text = row !== null ? row.fontFamily : ""
        sizeField.text = row !== null ? String(row.fontSize) : ""
        alignCombo.currentIndex = row !== null
                ? ["left", "center", "right"].indexOf(row.align) : 1
        spacingField.text = row !== null ? String(row.lineSpacing) : ""
    }
    // Every inspector edit writes the record back WHOLE — editTheme's
    // contract that one omitted field cannot silently flatten a gradient.
    // A value the model refuses lands on the status line, not in the data.
    function applyField(name, value) {
        const row = themeEditor.row
        if (row === null)
            return
        docs.editTheme(row.id,
                       name === "name" ? value : row.name,
                       name === "textColor" ? value : row.textColor,
                       name === "backgroundColor" ? value : row.backgroundColor,
                       name === "backgroundGradient" ? value : row.backgroundGradient,
                       name === "fontFamily" ? value : row.fontFamily,
                       name === "fontSize" ? value : row.fontSize,
                       name === "align" ? value : row.align,
                       name === "lineSpacing" ? value : row.lineSpacing)
    }

    // The centre picture: the worker's own composition of the built-in
    // sample wearing the selected theme. Debounced like the Edit page's
    // ask — browsing is a click-track, and one ask costs a prepare and an
    // off-air draw.
    Timer {
        id: sampleAsk
        interval: 120
        repeat: false
        onTriggered: {
            if (!themeEditor.visible || themeEditor.selectedId === "") {
                worker.clearSelectionPreview()
                return
            }
            const json = docs.themeSampleSceneJson(themeEditor.selectedId)
            if (json !== "")
                worker.requestSelectionPreview(json)
        }
    }
    function askForSamplePicture() {
        sampleAsk.restart()
    }
    Connections {
        target: docs
        function onDocumentsChanged() {
            themeEditor.reconcileSelection()
            // Re-derive the toggle from the model on every change: this is
            // the hook that keeps it honest when the More menu, not this
            // page, moved the current theme. (The user's own click already
            // broke the binding; this reassignment is what replaces it.)
            useToggle.checked = docs.wornThemeId !== ""
                                && docs.wornThemeId === themeEditor.selectedId
            if (themeEditor.visible)
                themeEditor.askForSamplePicture()
        }
    }
    onVisibleChanged: {
        if (themeEditor.visible) {
            themeEditor.reconcileSelection()
            themeEditor.askForSamplePicture()
        } else {
            sampleAsk.stop()
            worker.clearSelectionPreview()
            themeEditor.renamingId = ""
        }
    }
    Component.onCompleted: {
        themeEditor.reconcileSelection()
        themeEditor.syncInspector()
    }
    onSelectedIdChanged: themeEditor.syncInspector()

    /* --- the list ------------------------------------------------------- */

    Column {
        id: listColumn
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: 230
        spacing: 8

        Text { text: "THEMES"; color: "#bbbbbb"; font.pixelSize: 11; font.bold: true }
        ListView {
            id: themeList
            objectName: "themeEditorList"
            width: parent.width
            height: 340
            clip: true
            model: docs.themes
            delegate: Rectangle {
                id: themeRow
                required property var modelData
                objectName: "themeEditorRow"
                width: themeList.width
                height: 34
                radius: 4
                color: themeEditor.selectedId === themeRow.modelData.id ? "#626265"
                       : rowMouse.containsMouse ? "#59595b" : "transparent"
                property bool editing: themeEditor.renamingId === themeRow.modelData.id
                Row {
                    anchors.left: parent.left
                    anchors.leftMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 6
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 14
                        color: "#8ec9f5"
                        font.pixelSize: 13
                        text: docs.wornThemeId === themeRow.modelData.id ? "\u2713" : ""
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: !themeRow.editing
                        text: themeRow.modelData.name
                        color: "#eeeeee"
                        font.pixelSize: 13
                    }
                    PanelField {
                        objectName: "themeEditorRenameField"
                        visible: themeRow.editing
                        width: 140
                        text: themeRow.modelData.name
                        onVisibleChanged: if (visible) forceActiveFocus()
                        onEditingFinished: {
                            const clean = text.trim()
                            themeEditor.renamingId = ""
                            if (clean !== "" && clean !== themeRow.modelData.name) {
                                const model = themeRow.modelData
                                docs.editTheme(model.id, clean, model.textColor,
                                               model.backgroundColor, model.backgroundGradient,
                                               model.fontFamily, model.fontSize, model.align,
                                               model.lineSpacing)
                            }
                        }
                        Keys.onEscapePressed: themeEditor.renamingId = ""
                    }
                }
                MouseArea {
                    id: rowMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    // Selection is the single click; the rename is its own
                    // gesture, because Qt emits `clicked` once for a
                    // double-click too — on the release, without a flag —
                    // so a check on the press could only ever live here.
                    onClicked: themeEditor.selectTheme(themeRow.modelData.id)
                    onDoubleClicked: themeEditor.renamingId = themeRow.modelData.id
                }
            }
        }
        Row {
            spacing: 6
            Button {
                objectName: "themeEditorNew"
                text: "New"
                onClicked: docs.createBlankTheme("New theme")
            }
            Button {
                objectName: "themeEditorDuplicate"
                text: "Duplicate"
                enabled: themeEditor.selectedId !== ""
                onClicked: docs.duplicateTheme(themeEditor.selectedId)
            }
            Button {
                objectName: "themeEditorDelete"
                text: "Delete"
                enabled: themeEditor.selectedId !== ""
                onClicked: docs.deleteTheme(themeEditor.selectedId)
            }
        }
        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            color: "#b0b0b0"
            font.pixelSize: 11
            text: "Double-click a name to rename it. Deleting clears every "
                  + "reference to the theme; Ctrl+Z brings it back."
        }
    }

    /* --- the centre: header, sample, honesty line ----------------------- */

    Column {
        id: centerColumn
        anchors.left: listColumn.right
        anchors.leftMargin: 12
        anchors.right: inspectorColumn.left
        anchors.rightMargin: 12
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        spacing: 10

        Row {
            spacing: 14
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "Theme Editor"
                color: "#eeeeee"
                font.pixelSize: 18
                font.bold: true
            }
            CheckBox {
                id: useToggle
                objectName: "themeEditorUseToggle"
                anchors.verticalCenter: parent.verticalCenter
                text: "Use as current theme"
                font.pixelSize: 12
                enabled: themeEditor.selectedId !== ""
                // The toggle reads the model, not its own press: the More
                // menu can put a theme on or take it off from anywhere, and
                // a refusal must not leave a checked box claiming otherwise.
                checked: docs.wornThemeId !== ""
                         && docs.wornThemeId === themeEditor.selectedId
                onToggled: {
                    docs.useThemeAsCurrent(checked ? themeEditor.selectedId : "")
                    checked = docs.wornThemeId !== ""
                              && docs.wornThemeId === themeEditor.selectedId
                }
            }
        }

        Rectangle {
            id: previewFrame
            objectName: "themeEditorPreview"
            width: parent.width
            height: Math.min(width * 9 / 16, parent.height - 96)
            color: "#000000"
            border.color: "#4a4a4a"
            clip: true
            PreviewView {
                anchors.fill: parent
                controller: worker
                target: PreviewView.Selection
                visible: worker.selectionReady
            }
            // No picture (no worker, the first ask still in flight): the
            // frame says which, rather than painting a QML guess at it.
            Text {
                anchors.centerIn: parent
                visible: !worker.selectionReady
                color: "#888888"
                font.pixelSize: 12
                text: themeEditor.selectedId === "" ? "No theme selected"
                      : worker.statusNeedsAttention ? "Preview unavailable"
                                                    : "Waiting for the preview"
            }
        }
        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            color: "#b0b0b0"
            font.pixelSize: 11
            text: "A built-in sample cue wearing this theme — the same "
                  + "picture on every row. Browsing themes never touches "
                  + "the audience output."
        }
        Text {
            objectName: "themeEditorStatus"
            width: parent.width
            wrapMode: Text.WordWrap
            color: "#d9a441"
            font.pixelSize: 11
            visible: text !== ""
            text: docs.themes.length === 0 && themeEditor.selectedId === ""
                  ? "No themes yet — New makes one." : docs.status
        }
    }

    /* --- the inspector --------------------------------------------------- */

    Column {
        id: inspectorColumn
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: 250
        spacing: 8

        Text {
            text: "SELECTED THEME"
            color: "#bbbbbb"
            font.pixelSize: 11
            font.bold: true
        }
        Text { text: "Name"; color: "#b0b0b0"; font.pixelSize: 11 }
        PanelField {
            id: nameField
            objectName: "themeEditorNameField"
            width: parent.width
            onEditingFinished: themeEditor.applyField("name", text.trim())
        }
        Text { text: "Text colour (#rgb or #rrggbb)"; color: "#b0b0b0"; font.pixelSize: 11 }
        PanelField {
            id: colourField
            objectName: "themeEditorColourField"
            width: parent.width
            onEditingFinished: themeEditor.applyField("textColor", text)
        }
        Text { text: "Background colour"; color: "#b0b0b0"; font.pixelSize: 11 }
        PanelField {
            id: backField
            objectName: "themeEditorBackField"
            width: parent.width
            onEditingFinished: themeEditor.applyField("backgroundColor", text)
        }
        Text { text: "Gradient colour (\"\" = flat)"; color: "#b0b0b0"; font.pixelSize: 11 }
        PanelField {
            id: gradientField
            objectName: "themeEditorGradientField"
            width: parent.width
            onEditingFinished: themeEditor.applyField("backgroundGradient", text)
        }
        Text { text: "Font family (\"\" = platform)"; color: "#b0b0b0"; font.pixelSize: 11 }
        PanelField {
            id: fontField
            objectName: "themeEditorFontField"
            width: parent.width
            onEditingFinished: themeEditor.applyField("fontFamily", text)
        }
        Text { text: "Font size (0 = renderer default)"; color: "#b0b0b0"; font.pixelSize: 11 }
        PanelField {
            id: sizeField
            objectName: "themeEditorSizeField"
            width: parent.width
            onEditingFinished: themeEditor.applyField("fontSize", Number(text))
        }
        Text { text: "Alignment"; color: "#b0b0b0"; font.pixelSize: 11 }
        ComboBox {
            id: alignCombo
            objectName: "themeEditorAlign"
            width: parent.width
            model: ["left", "center", "right"]
            onActivated: themeEditor.applyField("align", currentValue)
        }
        Text { text: "Line spacing (0.25 to 8)"; color: "#b0b0b0"; font.pixelSize: 11 }
        PanelField {
            id: spacingField
            objectName: "themeEditorSpacingField"
            width: parent.width
            onEditingFinished: themeEditor.applyField("lineSpacing", Number(text))
        }
    }
}
