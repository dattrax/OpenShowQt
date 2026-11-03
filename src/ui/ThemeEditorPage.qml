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
 * Since task TF.4 the inspector also edits the theme's `scripture` reading
 * context — typography and a box only, never a colour and never a line
 * count; the "lines" note there is the worker's measurement, shown as
 * information (text-flow plan).
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
    // and after a commit the model is re-read whole anyway. The row is
    // re-read through rowFor rather than taken from the `row` property
    // because this function runs from inside the documentsChanged emit:
    // at that moment the property's binding may not have re-run yet, and
    // the cached row would replay the numbers the write just replaced.
    function syncInspector() {
        const row = themeEditor.rowFor(themeEditor.selectedId)
        nameField.text = row !== null ? row.name : ""
        colourField.text = row !== null ? row.textColor : ""
        backField.text = row !== null ? row.backgroundColor : ""
        gradientField.text = row !== null ? row.backgroundGradient : ""
        fontField.text = row !== null ? row.fontFamily : ""
        sizeField.text = row !== null ? String(row.fontSize) : ""
        alignCombo.currentIndex = row !== null
                ? ["left", "center", "right"].indexOf(row.align) : 1
        spacingField.text = row !== null ? String(row.lineSpacing) : ""
        // The placement fields follow the box the overlay shows (the theme's
        // own, or the built-in lyric box it is being offered).
        const box = themeEditor.boxFor(row)
        placeXField.text = String(Math.round(box.x))
        placeYField.text = String(Math.round(box.y))
        placeWField.text = String(Math.round(box.width))
        placeHField.text = String(Math.round(box.height))
        themeEditor.syncContext()
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

    /* --- the scripture reading context (task TF.4, text-flow plan) -------
     * The entry this theme carries under the name "scripture" decides how
     * a Bible passage is SET: typography and a box, and nothing else — no
     * colour (the theme's look is the theme's own) and never a line count,
     * because how many lines ride a slide is what the worker's measurement
     * says, not what anyone types here. The fields read and write that one
     * entry through the controller's verbs; an empty field is SILENCE (the
     * property falls through to the themes), and the "lines" note is the
     * same engine's answer for the same box the Bible page will paginate
     * into, shown as information only.
     */
    // The measure request id of the in-flight lines probe (0 = none).
    property int linesAskId: 0
    // Whether the selected theme carries the context (derived on every
    // sync; the toggle follows the record, never the other way round).
    property bool contextOn: false

    function themeRow() {
        return themeEditor.rowFor(themeEditor.selectedId)
    }
    function contextRecord() {
        if (themeEditor.selectedId === "" || themeEditor.themeRow() === null)
            return {set: false}
        return docs.themeContext(themeEditor.selectedId, "scripture")
    }
    // The look the passage actually resolves: the context where it speaks,
    // the theme and the landing box where it is silent. The lines probe
    // measures THIS, which is the same chain the Bible page measures.
    function contextTypography() {
        const row = themeEditor.themeRow()
        const ctx = themeEditor.contextRecord()
        return {
            size: ctx.fontSizeSet === true ? ctx.fontSize : (row !== null ? row.fontSize : 0),
            spacing: ctx.lineSpacingSet === true ? ctx.lineSpacing
                                                 : (row !== null ? row.lineSpacing : 1.25),
            box: ctx.boxSet === true ? Qt.rect(ctx.x, ctx.y, ctx.width, ctx.height)
                                     : themeEditor.boxFor(row)
        }
    }
    // Re-read the whole section from the record — the commit, the More
    // menu's undo, a refused write: whatever moved, the fields follow.
    function syncContext() {
        const row = themeEditor.themeRow()
        const ctx = themeEditor.contextRecord()
        themeEditor.contextOn = ctx.set === true
        ctxToggle.checked = ctx.set === true
        ctxFontField.text = ctx.fontFamilySet === true ? ctx.fontFamily : ""
        ctxSizeField.text = ctx.fontSizeSet === true ? String(ctx.fontSize) : ""
        ctxSpacingField.text = ctx.lineSpacingSet === true ? String(ctx.lineSpacing) : ""
        ctxAlignCombo.currentIndex = ctx.alignSet === true
                ? ["left", "center", "right"].indexOf(ctx.align) + 1 : 0
        // Silence SHOWS the box the passage would land on anyway — the
        // theme's lyric rectangle, or the built-in one — because that is
        // the honest answer to "where do the words go right now"; typing
        // over it states the context's own opinion.
        const box = ctx.boxSet === true
                ? Qt.rect(ctx.x, ctx.y, ctx.width, ctx.height) : themeEditor.boxFor(row)
        ctxXField.text = String(Math.round(box.x))
        ctxYField.text = String(Math.round(box.y))
        ctxWField.text = String(Math.round(box.width))
        ctxHField.text = String(Math.round(box.height))
        linesAsk.restart()
    }
    // Toggle on CREATES the context from this theme's current look and box:
    // the reading starts as what the audience already sees, and the
    // operator narrows it to a lectern from there. Toggle off removes the
    // entry; a refusal (there was nothing to remove) leaves the toggle
    // exactly where the record says it is.
    function setContext(on) {
        const row = themeEditor.themeRow()
        if (row === null) {
            ctxToggle.checked = themeEditor.contextOn
            return
        }
        if (on) {
            const box = themeEditor.boxFor(row)
            const fields = {}
            if (row.fontFamily !== "")
                fields.fontFamily = row.fontFamily
            if (row.fontSize > 0)
                fields.fontSize = row.fontSize
            fields.lineSpacing = row.lineSpacing
            fields.align = row.align
            fields.box = {x: box.x, y: box.y, width: box.width, height: box.height}
            if (!docs.applyThemeContext(row.id, "scripture", fields))
                ctxToggle.checked = false
        } else if (!docs.removeThemeContext(row.id, "scripture")) {
            ctxToggle.checked = true
        }
    }
    // A field's commit is a WHOLE-entry write: the current set merged with
    // the one field, because the domain, like the file, replaces the entry
    // it is given. null is the field's way of saying "be silent again".
    function commitContext(overrides) {
        const id = themeEditor.selectedId
        const current = themeEditor.contextRecord()
        if (id === "" || current.set !== true)
            return
        const fields = {}
        if (current.fontFamilySet === true)
            fields.fontFamily = current.fontFamily
        if (current.fontSizeSet === true)
            fields.fontSize = current.fontSize
        if (current.lineSpacingSet === true)
            fields.lineSpacing = current.lineSpacing
        if (current.alignSet === true)
            fields.align = current.align
        if (current.insetSet === true)
            fields.inset = current.inset
        if (current.boxSet === true)
            fields.box = {x: current.x, y: current.y, width: current.width,
                          height: current.height}
        for (const key in overrides) {
            if (overrides[key] === null)
                delete fields[key]
            else
                fields[key] = overrides[key]
        }
        if (!docs.applyThemeContext(id, "scripture", fields))
            themeEditor.syncContext()  // the refusal wins: the fields go back
    }
    // Four fields commit together, exactly like the theme's own placement:
    // a box is one unit, and a half-typed one is a wrong one.
    function applyContextBox() {
        const x = parseFloat(ctxXField.text)
        const y = parseFloat(ctxYField.text)
        const w = parseFloat(ctxWField.text)
        const h = parseFloat(ctxHField.text)
        if (!isFinite(x) || !isFinite(y) || !isFinite(w) || !isFinite(h) || w <= 0 || h <= 0) {
            themeEditor.syncContext()
            return
        }
        themeEditor.commitContext({box: {x: x, y: y, width: w, height: h}})
    }

    /* --- the placement (themes plan amendment 2026-11-01) ---------------
     *
     * Where this theme puts a wearing slide's PRIMARY lyric box, edited with
     * the slide editor's own gestures: press inside to move, a corner to
     * resize, ONE write on release, and the numbers can be typed instead.
     * Absent is a real answer, so the overlay shows the built-in lyric box
     * as an OFFER while the theme places nothing, and the first drag (or the
     * first typed value) makes the placement exist. None of this reaches the
     * audience: the sample is a fixture, and the theme's own rectangle is all
     * that changes — the same boundary the colour fields keep.
     */
    readonly property bool hasPlacement: themeEditor.row !== null
                                         && themeEditor.row.placementSet === true
    // The gesture's live rectangle, in the 1920x1080 authored canvas.
    property bool placing: false
    property string dragMode: ""
    property real dragOriginX: 0
    property real dragOriginY: 0
    property real penX: 0
    property real penY: 0
    property real penW: 0
    property real penH: 0
    property real anchorX: 0
    property real anchorY: 0
    property real anchorW: 0
    property real anchorH: 0
    function clamp(value, lo, hi) {
        return value < lo ? lo : (value > hi ? hi : value)
    }
    // What the overlay draws and the fields show: the live drag while a
    // gesture runs, the theme's own rectangle otherwise, and the built-in
    // lyric box (the one a slide with no theme draws) as the offer when the
    // theme has no opinion yet.
    function boxFor(row) {
        if (row !== null && row.placementSet)
            return Qt.rect(row.placement.x, row.placement.y,
                           row.placement.width, row.placement.height)
        return Qt.rect(160, 100, 1600, 880)
    }
    readonly property rect shownBox: themeEditor.placing
            ? Qt.rect(themeEditor.penX, themeEditor.penY,
                      themeEditor.penW, themeEditor.penH)
            : themeEditor.boxFor(themeEditor.row)
    // Handles beat the interior, in preview pixels, with the Edit canvas's
    // 8 px tolerance and its 20 authored px floor: below that a box cannot
    // be grabbed back by its own handles.
    function hitTest(px, py) {
        if (themeEditor.row === null)
            return ""
        const zoom = placementLayer.zoom
        const box = themeEditor.shownBox
        const tol = 8
        const left = box.x * zoom
        const top = box.y * zoom
        const right = (box.x + box.width) * zoom
        const bottom = (box.y + box.height) * zoom
        if (Math.abs(px - left) <= tol && Math.abs(py - top) <= tol) return "tl"
        if (Math.abs(px - right) <= tol && Math.abs(py - top) <= tol) return "tr"
        if (Math.abs(px - left) <= tol && Math.abs(py - bottom) <= tol) return "bl"
        if (Math.abs(px - right) <= tol && Math.abs(py - bottom) <= tol) return "br"
        if (px >= left && px <= right && py >= top && py <= bottom) return "move"
        return ""
    }
    function beginGesture(mode) {
        const box = themeEditor.shownBox
        themeEditor.dragMode = mode
        themeEditor.anchorX = box.x
        themeEditor.anchorY = box.y
        themeEditor.anchorW = box.width
        themeEditor.anchorH = box.height
        themeEditor.penX = box.x
        themeEditor.penY = box.y
        themeEditor.penW = box.width
        themeEditor.penH = box.height
        themeEditor.placing = true
    }
    function moveGesture(dx, dy) {
        if (themeEditor.dragMode === "move") {
            themeEditor.penX = themeEditor.clamp(themeEditor.anchorX + dx, 0,
                                                 1920 - themeEditor.anchorW)
            themeEditor.penY = themeEditor.clamp(themeEditor.anchorY + dy, 0,
                                                 1080 - themeEditor.anchorH)
            return
        }
        const min = 20
        const right = themeEditor.anchorX + themeEditor.anchorW
        const bottom = themeEditor.anchorY + themeEditor.anchorH
        const x0 = (themeEditor.dragMode === "tl" || themeEditor.dragMode === "bl")
            ? themeEditor.clamp(themeEditor.anchorX + dx, 0, right - min)
            : themeEditor.anchorX
        const y0 = (themeEditor.dragMode === "tl" || themeEditor.dragMode === "tr")
            ? themeEditor.clamp(themeEditor.anchorY + dy, 0, bottom - min)
            : themeEditor.anchorY
        themeEditor.penX = x0
        themeEditor.penY = y0
        themeEditor.penW = right - x0
        themeEditor.penH = bottom - y0
    }
    // One history entry per gesture, on release — the Edit canvas's rule,
    // so dragging a theme's box is one undo and not one per pixel.
    function endGesture() {
        if (themeEditor.dragMode === "") {
            themeEditor.placing = false
            return
        }
        const id = themeEditor.selectedId
        themeEditor.placing = false
        themeEditor.dragMode = ""
        if (id === "")
            return
        docs.setThemePlacement(id, Math.round(themeEditor.penX), Math.round(themeEditor.penY),
                               Math.round(themeEditor.penW), Math.round(themeEditor.penH))
        themeEditor.syncInspector()
    }
    // The exact-number path. Four fields commit together because a placement
    // is one unit; anything the model refuses (or that is not a number) puts
    // the fields back where the model says they belong.
    function applyPlacementFields() {
        const row = themeEditor.row
        const x = parseFloat(placeXField.text)
        const y = parseFloat(placeYField.text)
        const w = parseFloat(placeWField.text)
        const h = parseFloat(placeHField.text)
        if (row === null || !Number.isFinite(x) || !Number.isFinite(y)
                || !(w >= 1) || !(h >= 1)
                || !docs.setThemePlacement(row.id, x, y, w, h))
            themeEditor.syncInspector()
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
    // The measured lines the passage's box holds (information, never a
    // setting): one "M" at the resolved size and spacing, measured by the
    // worker's own paragraph engine — the same one the Bible page paginates
    // with — and the box height divided by the row it answers with.
    Timer {
        id: linesAsk
        interval: 240
        repeat: false
        onTriggered: {
            const look = themeEditor.contextTypography()
            if (!themeEditor.visible || !themeEditor.contextOn) {
                ctxLinesLabel.text = ""
                return
            }
            if (!worker.connected) {
                ctxLinesLabel.text = "not measured — the worker is not answering"
                return
            }
            // A size of 0 is the renderer's own default, which no probe
            // here can name — say so rather than divide by a guess.
            if (look.size <= 0) {
                ctxLinesLabel.text = "the renderer's default size decides"
                return
            }
            const id = worker.measureText([{
                key: "theme-editor-lines",
                lines: ["M"],
                fontSize: look.size,
                width: look.box.width,
                lineSpacing: look.spacing
            }])
            themeEditor.linesAskId = id
            if (id === 0)
                ctxLinesLabel.text = "not measured — the worker is not answering"
        }
    }
    Connections {
        target: worker
        function onTextMeasured(requestId, measured) {
            if (requestId !== themeEditor.linesAskId || measured.length === 0)
                return  // another page's batch; the editor asked nothing
            themeEditor.linesAskId = 0
            const rowHeight = Number(measured[0].height)
            const box = themeEditor.contextTypography().box
            ctxLinesLabel.text = rowHeight > 0
                    ? "the box holds ≈ " + Math.floor(box.height / rowHeight)
                      + " lines of this size — how many ride is what the passage measures"
                    : ""
        }
        function onTextMeasureFailed(requestId, reason) {
            if (requestId !== themeEditor.linesAskId)
                return
            themeEditor.linesAskId = 0
            ctxLinesLabel.text = "not measured: " + reason
        }
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
            // The inspector reads the RECORD: whatever moved it — this
            // page's own commit, "No placement", an undo — the fields and
            // the overlay follow, so a stale number can never outlive the
            // write it was typed for.
            if (themeEditor.visible) {
                themeEditor.syncInspector()
                themeEditor.askForSamplePicture()
            }
        }
    }
    onVisibleChanged: {
        if (themeEditor.visible) {
            themeEditor.reconcileSelection()
            // Shown pages read the RECORD: documents may have moved while
            // this page slept (another page, an undo, the More menu), and
            // a selection that never changed fires no selectedIdChanged to
            // re-sync the assigned fields. The overlay follows by binding;
            // the fields only follow through here.
            themeEditor.syncInspector()
            themeEditor.askForSamplePicture()
        } else {
            sampleAsk.stop()
            linesAsk.stop()
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
                         // The seeded Default theme (amendment 2026-11-02)
                         // is the floor every chain resolves through:
                         // editable like any theme, but never deleted.
                         && !(themeEditor.row && themeEditor.row.builtin === true)
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

            // The lyric box this theme places (themes plan amendment
            // 2026-11-01), drawn over the sample at the same 1920-unit scale
            // the Edit canvas uses. It is chrome over the worker's own
            // picture, never a second layout of the words — and with no
            // placement on the theme it is a dashed OFFER, the box the
            // audience would draw today, waiting to be moved into being.
            Item {
                id: placementLayer
                anchors.fill: parent
                enabled: themeEditor.row !== null
                visible: enabled
                readonly property real zoom: width / 1920
                readonly property rect box: themeEditor.shownBox

                Rectangle {
                    x: placementLayer.box.x * placementLayer.zoom
                    y: placementLayer.box.y * placementLayer.zoom
                    width: placementLayer.box.width * placementLayer.zoom
                    height: placementLayer.box.height * placementLayer.zoom
                    color: "transparent"
                    // A Rectangle border is only ever solid, and the solid/dashed
                    // difference here is the whole message — "this is the record"
                    // versus "this is only the box you are being offered". Canvas
                    // is the one QtQuick type that dashes a stroke without a new
                    // module import, so the outline is its own honest paint.
                    Canvas {
                        anchors.fill: parent
                        readonly property bool placed: themeEditor.hasPlacement
                        onPlacedChanged: requestPaint()
                        onWidthChanged: requestPaint()
                        onHeightChanged: requestPaint()
                        onPaint: {
                            const ctx = getContext("2d")
                            ctx.clearRect(0, 0, width, height)
                            ctx.strokeStyle = placed ? "#1685f8" : "#8ec9f5"
                            ctx.lineWidth = 2
                            ctx.setLineDash(placed ? [] : [6, 4])
                            ctx.strokeRect(1, 1, width - 2, height - 2)
                        }
                    }
                    Text {
                        anchors.left: parent.left
                        anchors.bottom: parent.top
                        anchors.bottomMargin: 3
                        color: themeEditor.hasPlacement ? "#1685f8" : "#8ec9f5"
                        font.pixelSize: 11
                        text: themeEditor.hasPlacement ? "lyric box" : "no placement — drag to place"
                    }
                    Repeater {
                        model: [Qt.point(0, 0), Qt.point(1, 0), Qt.point(0, 1), Qt.point(1, 1)]
                        delegate: Rectangle {
                            required property var modelData
                            width: 8
                            height: 8
                            color: themeEditor.hasPlacement ? "#1685f8" : "#8ec9f5"
                            border.color: "#202020"
                            x: placementLayer.box.x * placementLayer.zoom
                                + (modelData.x === 0 ? -width / 2
                                                     : placementLayer.box.width
                                                       * placementLayer.zoom - width / 2)
                            y: placementLayer.box.y * placementLayer.zoom
                                + (modelData.y === 0 ? -height / 2
                                                     : placementLayer.box.height
                                                       * placementLayer.zoom - height / 2)
                        }
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.LeftButton
                    onPressed: function (mouse) {
                        const mode = themeEditor.hitTest(mouse.x, mouse.y)
                        if (mode !== "") {
                            themeEditor.beginGesture(mode)
                            themeEditor.dragOriginX = mouse.x
                            themeEditor.dragOriginY = mouse.y
                        }
                    }
                    onPositionChanged: function (mouse) {
                        if (themeEditor.dragMode !== "")
                            themeEditor.moveGesture((mouse.x - themeEditor.dragOriginX)
                                                    / placementLayer.zoom,
                                                    (mouse.y - themeEditor.dragOriginY)
                                                    / placementLayer.zoom)
                    }
                    onReleased: themeEditor.endGesture()
                    onCanceled: themeEditor.endGesture()
                }
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

        // The lyric box this theme places (themes plan amendment
        // 2026-11-01): the same rectangle the overlay above is showing, in
        // the 1920x1080 authored canvas, for the operator who would rather
        // type it. Four fields commit together — a placement is ONE unit —
        // and "No placement" takes the theme's opinion away so wearing
        // slides draw the boxes they were authored with.
        Text {
            text: "Lyric box (x, y, w, h in 1920×1080)"
            color: "#b0b0b0"
            font.pixelSize: 11
        }
        Row {
            spacing: 4
            PanelField {
                id: placeXField
                objectName: "themeEditorPlaceX"
                width: 52
                onEditingFinished: themeEditor.applyPlacementFields()
            }
            PanelField {
                id: placeYField
                objectName: "themeEditorPlaceY"
                width: 52
                onEditingFinished: themeEditor.applyPlacementFields()
            }
            PanelField {
                id: placeWField
                objectName: "themeEditorPlaceW"
                width: 52
                onEditingFinished: themeEditor.applyPlacementFields()
            }
            PanelField {
                id: placeHField
                objectName: "themeEditorPlaceH"
                width: 52
                onEditingFinished: themeEditor.applyPlacementFields()
            }
        }
        Row {
            spacing: 6
            PanelButton {
                objectName: "themeEditorNoPlacement"
                text: "No placement"
                enabled: themeEditor.hasPlacement
                onClicked: docs.clearThemePlacement(themeEditor.selectedId)
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                color: themeEditor.hasPlacement ? "#8ec9f5" : "#888888"
                font.pixelSize: 11
                text: themeEditor.hasPlacement ? "this theme places its slides"
                                               : "no opinion — slides keep their own boxes"
            }
        }

        // The scripture reading context (task TF.4): the one named entry
        // that says how a Bible passage is SET by this theme. Typography
        // and a box, nothing else — the colours stay the theme's and the
        // line count stays the measurement's.
        Item { width: parent.width; height: 6 }
        Text {
            text: "SCRIPTURE READING"
            color: "#bbbbbb"
            font.pixelSize: 11
            font.bold: true
        }
        CheckBox {
            id: ctxToggle
            objectName: "themeEditorContextToggle"
            text: "set passages apart"
            font.pixelSize: 11
            enabled: themeEditor.selectedId !== ""
            checked: false
            onToggled: themeEditor.setContext(checked)
        }
        Column {
            objectName: "themeEditorContextFields"
            width: parent.width
            spacing: 8
            visible: themeEditor.contextOn
            Text { text: "Reading font (\"\" = theme's)"; color: "#b0b0b0"; font.pixelSize: 11 }
            PanelField {
                id: ctxFontField
                objectName: "themeEditorContextFont"
                width: parent.width
                onEditingFinished: themeEditor.commitContext(
                            {fontFamily: text.trim() === "" ? null : text.trim()})
            }
            Text { text: "Reading size (0 = renderer, \"\" = theme's)"
                       color: "#b0b0b0"; font.pixelSize: 11 }
            PanelField {
                id: ctxSizeField
                objectName: "themeEditorContextSize"
                width: parent.width
                onEditingFinished: {
                    const t = text.trim()
                    if (t === "") {
                        themeEditor.commitContext({fontSize: null})
                        return
                    }
                    const v = Number(t)
                    if (!isFinite(v) || v < 0) {
                        themeEditor.syncContext()
                        return
                    }
                    themeEditor.commitContext({fontSize: v})
                }
            }
            Text { text: "Reading alignment"; color: "#b0b0b0"; font.pixelSize: 11 }
            ComboBox {
                id: ctxAlignCombo
                objectName: "themeEditorContextAlign"
                width: parent.width
                model: ["(theme)", "left", "center", "right"]
                onActivated: themeEditor.commitContext(
                            {align: currentIndex === 0 ? null : currentValue})
            }
            Text { text: "Reading line spacing (0.25 to 8, \"\" = theme's)"
                       color: "#b0b0b0"; font.pixelSize: 11 }
            PanelField {
                id: ctxSpacingField
                objectName: "themeEditorContextSpacing"
                width: parent.width
                onEditingFinished: {
                    const t = text.trim()
                    if (t === "") {
                        themeEditor.commitContext({lineSpacing: null})
                        return
                    }
                    const v = Number(t)
                    if (!isFinite(v) || v < 0.25 || v > 8) {
                        themeEditor.syncContext()
                        return
                    }
                    themeEditor.commitContext({lineSpacing: v})
                }
            }
            Text { text: "Reading box (x, y, w, h in 1920×1080)"
                       color: "#b0b0b0"; font.pixelSize: 11 }
            Row {
                spacing: 4
                PanelField {
                    id: ctxXField
                    objectName: "themeEditorContextX"
                    width: 52
                    onEditingFinished: themeEditor.applyContextBox()
                }
                PanelField {
                    id: ctxYField
                    objectName: "themeEditorContextY"
                    width: 52
                    onEditingFinished: themeEditor.applyContextBox()
                }
                PanelField {
                    id: ctxWField
                    objectName: "themeEditorContextW"
                    width: 52
                    onEditingFinished: themeEditor.applyContextBox()
                }
                PanelField {
                    id: ctxHField
                    objectName: "themeEditorContextH"
                    width: 52
                    onEditingFinished: themeEditor.applyContextBox()
                }
            }
            Row {
                spacing: 6
                PanelButton {
                    objectName: "themeEditorContextBoxTheme"
                    text: "Theme's lyric box"
                    onClicked: {
                        const box = themeEditor.boxFor(themeEditor.themeRow())
                        themeEditor.commitContext({box: {x: box.x, y: box.y,
                                                         width: box.width, height: box.height}})
                    }
                }
                PanelButton {
                    objectName: "themeEditorContextBoxClear"
                    text: "No box"
                    onClicked: themeEditor.commitContext({box: null})
                }
            }
            Text {
                id: ctxLinesLabel
                objectName: "themeEditorContextLines"
                width: parent.width
                wrapMode: Text.WordWrap
                color: "#8ec9f5"
                font.pixelSize: 11
                text: ""
            }
            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                color: "#888888"
                font.pixelSize: 10
                text: "a reading context sets type and a box — never a colour, "
                      + "and never a line count: how many lines ride a slide is "
                      + "what the renderer measures"
            }
        }
    }
}
