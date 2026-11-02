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
    }
}
