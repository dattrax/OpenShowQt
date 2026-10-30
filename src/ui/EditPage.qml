import QtQuick
import QtQuick.Controls

/*
 * The edit page (task 4.3, stack since 6.17a2; task 6.19b7 out of
 * Main.qml): the current slide's boxes on a 16:9 stage — click selects
 * the topmost box, drag moves it, corners resize it, Tab walks the
 * stack, arrows nudge — with exact X/Y/W/H fields and
 * New/Delete/Back/Forward under it as the deterministic path, and the
 * style panel beside it. One undo entry per drag, per nudge, per field
 * commit, and per stack command.
 *
 * The canvas's live rectangle is the one fact here the window cannot
 * reach any other way — the nudge commits the same geometry the drag
 * commits, and only the canvas tracks a half-finished one — so the two
 * key executors that read it, nudgeBox() and stepBox(), travel with the
 * page they operate on, and the page exposes the selection flag as
 * boxSelected: the key map's two Escape owners hand off on it, and
 * Escape's deselect lands through the same alias. Everything else is
 * the page's own: the debounced ask for the worker's picture of the
 * slide under edit, the geometry fields, the stack commands and the
 * style editor, all addressing `docs` and `worker` directly.
 */
Item {
    id: editPage

    // The canvas's handles being on (or Escape having stepped out).
    property alias boxSelected: editCanvas.selected

    // Task 4.3: the reference's nudge rule (EditTools.svelte:314-326).
    // Since 6.17a2 "the selected box" is the session's selection: the
    // canvas mirrors its rectangle, and arrows, drag, fields and inspector
    // all move the box that wears the handles.
    function nudgeBox(dx, dy) {
        if (docs.currentSlide < 0 || docs.slides.length === 0) {
            return
        }
        docs.setTextBoxGeometry(editCanvas.boxX + dx, editCanvas.boxY + dy,
                                editCanvas.boxW, editCanvas.boxH)
    }
    // Task 6.17a2: step the selection through the slide's stack, wrapping.
    function stepBox(delta) {
        const count = editCanvas.boxes.length
        if (count === 0) {
            return
        }
        docs.selectTextBox((docs.currentBox + delta + count) % count)
    }


    EditCanvas {
        id: editCanvas
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: stylePanel.left
        anchors.rightMargin: 12
        anchors.bottom: boxFields.top
        anchors.bottomMargin: 12
    }

    /*
     * Asking the worker for its picture of the slide under edit (task
     * 6.16a, review R16). Debounced: one ask costs a scene.prepare and an
     * off-air draw, so a keystroke at a time would be renders nobody has
     * looked at yet. The triggers below are everything that can change
     * what the canvas should show — text, style, geometry (a drag
     * commits on release, which is a document change), navigation and
     * page entry — and the payload is compiled at the tick, so the
     * render is of the slide as it ended up rather than as it was when
     * typing started.
     */
    Timer {
        id: selectionAsk
        interval: 250
        repeat: false
        onTriggered: {
            if (!editPage.visible || !editCanvas.hasSlide) {
                worker.clearSelectionPreview()
                return
            }
            worker.requestSelectionPreview(
                docs.sceneJsonForSlide(docs.slides[docs.currentSlide].id))
        }
    }
    function askForWorkerPicture() {
        selectionAsk.restart()
    }
    Connections {
        target: docs
        function onDocumentsChanged() {
            if (editPage.visible) {
                editPage.askForWorkerPicture()
            }
        }
        function onSlideTextChanged() {
            if (editPage.visible) {
                editPage.askForWorkerPicture()
            }
        }
        function onNavigationChanged() {
            if (editPage.visible) {
                editPage.askForWorkerPicture()
            }
        }
    }
    // Leaving the page stands the target down: the worker stops drawing
    // (and holding a scene for) a slide nobody is looking at, and the
    // stream goes back to mirroring the audience alone.
    onVisibleChanged: {
        if (editPage.visible) {
            editPage.askForWorkerPicture()
        } else {
            selectionAsk.stop()
            worker.clearSelectionPreview()
        }
    }

    Row {
        id: boxFields
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        spacing: 8

        // The fields show the live box; the one that loses focus
        // commits all four. A value the domain refuses (or that is
        // not a number at all) snaps every field back.
        function commit() {
            const x = parseFloat(fieldX.text)
            const y = parseFloat(fieldY.text)
            const w = parseFloat(fieldW.text)
            const h = parseFloat(fieldH.text)
            // The exact-number path onto the SELECTED box — the same
            // op the drag commits, so one entry lands either way.
            if (!Number.isFinite(x) || !Number.isFinite(y)
                    || !(w >= 1) || !(h >= 1)
                    || !docs.setTextBoxGeometry(x, y, w, h)) {
                boxFields.sync()
            }
        }
        function sync() {
            fieldX.text = Qt.binding(function () { return editCanvas.boxX.toFixed(0) })
            fieldY.text = Qt.binding(function () { return editCanvas.boxY.toFixed(0) })
            fieldW.text = Qt.binding(function () { return editCanvas.boxW.toFixed(0) })
            fieldH.text = Qt.binding(function () { return editCanvas.boxH.toFixed(0) })
        }
        Component.onCompleted: sync()
        Connections {
            target: docs
            function onDocumentsChanged() { boxFields.sync() }
            function onNavigationChanged() { boxFields.sync() }
        }
        Connections {
            target: editPage
            function onVisibleChanged() { if (editPage.visible) boxFields.sync() }
        }

        Column {
            spacing: 2
            Text {
                color: "#8f93a5"
                font.pixelSize: 10
                text: "X"
            }
            TextField {
                id: fieldX
                objectName: "geometryField"
                width: 72
                color: "#f0f0ff"
                font.pixelSize: 13
                onEditingFinished: boxFields.commit()
                background: Rectangle {
                    color: "#12121c"
                    border.color: "#2f3542"
                }
            }
        }

        Column {
            spacing: 2
            Text {
                color: "#8f93a5"
                font.pixelSize: 10
                text: "Y"
            }
            TextField {
                id: fieldY
                objectName: "geometryField"
                width: 72
                color: "#f0f0ff"
                font.pixelSize: 13
                onEditingFinished: boxFields.commit()
                background: Rectangle {
                    color: "#12121c"
                    border.color: "#2f3542"
                }
            }
        }

        Column {
            spacing: 2
            Text {
                color: "#8f93a5"
                font.pixelSize: 10
                text: "W"
            }
            TextField {
                id: fieldW
                objectName: "geometryField"
                width: 72
                color: "#f0f0ff"
                font.pixelSize: 13
                onEditingFinished: boxFields.commit()
                background: Rectangle {
                    color: "#12121c"
                    border.color: "#2f3542"
                }
            }
        }

        Column {
            spacing: 2
            Text {
                color: "#8f93a5"
                font.pixelSize: 10
                text: "H"
            }
            TextField {
                id: fieldH
                objectName: "geometryField"
                width: 72
                color: "#f0f0ff"
                font.pixelSize: 13
                onEditingFinished: boxFields.commit()
                background: Rectangle {
                    color: "#12121c"
                    border.color: "#2f3542"
                }
            }
        }

        // The stack's commands (task 6.17a2). One undo step each, and
        // their enabled state reads the SELECTION: Delete takes the
        // selected box away, Back/Forward re-order it in the stack the
        // worker paints — what moves forward lands on top of the
        // audience's picture, exactly where the outlines here say it
        // will. "New" stays live at the ceiling and lets the status
        // line name the limit; the refusal adds no undo step.
        Column {
            spacing: 2
            Text {
                color: "#8f93a5"
                font.pixelSize: 10
                text: "STACK"
            }
            Row {
                spacing: 4
                PanelButton {
                    objectName: "newBoxButton"
                    text: "New box"
                    onClicked: docs.addTextBox()
                }
                PanelButton {
                    objectName: "deleteBoxButton"
                    text: "Delete"
                    enabled: docs.currentBox >= 0
                    onClicked: docs.removeCurrentTextBox()
                }
                PanelButton {
                    objectName: "boxBackButton"
                    text: "Back"
                    enabled: docs.currentBox > 0
                    onClicked: docs.moveCurrentTextBox(docs.currentBox - 1)
                }
                PanelButton {
                    objectName: "boxForwardButton"
                    text: "Forward"
                    enabled: docs.currentBox >= 0
                             && docs.currentBox < docs.currentSlideBoxes.length - 1
                    onClicked: docs.moveCurrentTextBox(docs.currentBox + 1)
                }
            }
        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            color: "#6a6f80"
            font.pixelSize: 11
            text: "drag a box · corners resize · Tab next box · arrows nudge 1px, Ctrl 10px"
        }
    }

    /*
     * The style panel (task 4.4): one style object is under edit —
     * the one the current slide resolves through, override or show
     * default. Editing it changes every slide that follows it, which
     * is the reuse the reference gives its themes; New forks the
     * look for this slide alone, Show default hands it to every
     * un-overridden slide, and Follow show (the empty id) gives the
     * override back. Fields commit on focus-out and snap back on a
     * refusal, exactly like the box fields beside them.
     */
    Column {
        id: stylePanel
        objectName: "stylePanel"
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        width: 250
        spacing: 10

        // The style the fields edit right now ("" = none anywhere).
        function targetId() {
            return docs.currentStyle.id || ""
        }
        // Combo row 0 is "follow show" (the empty id); the rest name
        // the styles view's rows in order.
        function styleIdAt(index) {
            if (index <= 0) {
                return ""
            }
            var i = index - 1
            return i < docs.styles.length ? docs.styles[i].id : ""
        }
        // Name, colours, font and typography commit as one edit.
        // A slide following nothing creates its style on first edit.
        function commit(newAlign) {
            const size = parseFloat(fieldStyleSize.text)
            const spacing = parseFloat(fieldLineSpacing.text)
            const align = newAlign !== undefined ? newAlign
                                                 : (docs.currentStyle.align || "center")
            const name = fieldStyleName.text.trim() || "Style"
            const colour = fieldTextColor.text.trim()
            const back = fieldBackColour.text.trim()
            // "" is a real answer here, not a missing value: it is how
            // the operator says the background stays flat (6.17c3).
            const gradient = fieldBackGradient.text.trim()
            const family = fieldFontFamily.text.trim()
            const spacingValue = Number.isFinite(spacing) ? spacing : 1.25
            const sizeValue = Number.isFinite(size) && size >= 0 ? size : 0
            if (targetId() === "") {
                if (!docs.createStyleFromCurrent(name)) {
                    stylePanel.sync()
                    return
                }
                // The creation took the built-in look; carry the
                // operator's values onto it, undoing the creation too
                // if the domain refuses them.
                if (!docs.editStyle(targetId(), name, colour, back, gradient, family,
                                    sizeValue, align, spacingValue)) {
                    docs.undo()
                    stylePanel.sync()
                }
                return
            }
            if (!docs.editStyle(targetId(), name, colour, back, gradient, family,
                                sizeValue, align, spacingValue)) {
                stylePanel.sync()
            }
        }
        function sync() {
            fieldStyleName.text = Qt.binding(function () {
                return docs.currentStyle.name || ""
            })
            fieldTextColor.text = Qt.binding(function () {
                return docs.currentStyle.textColor || "#ffffff"
            })
            fieldBackColour.text = Qt.binding(function () {
                return docs.currentStyle.backgroundColor || "#000000"
            })
            // Flat shows empty, not a colour: the field IS the "" that
            // commit passes back (6.17c3).
            fieldBackGradient.text = Qt.binding(function () {
                return docs.currentStyle.backgroundGradient || ""
            })
            fieldFontFamily.text = Qt.binding(function () {
                return docs.currentStyle.fontFamily || ""
            })
            fieldStyleSize.text = Qt.binding(function () {
                const size = docs.currentStyle.fontSize
                return size === undefined ? "0" : String(size)
            })
            fieldLineSpacing.text = Qt.binding(function () {
                const spacing = docs.currentStyle.lineSpacing
                return spacing === undefined ? "1.25" : String(spacing)
            })
            // The combo names the followed default as its first row;
            // an override selects its own style instead.
            var index = 0
            if (docs.currentStyleIsOverride) {
                for (var i = 0; i < docs.styles.length; ++i) {
                    if (docs.styles[i].id === stylePanel.targetId()) {
                        index = i + 1
                        break
                    }
                }
            }
            styleCombo.currentIndex = index
            var background = ""
            if (docs.currentSlide >= 0 && docs.slides.length > docs.currentSlide) {
                background = docs.slides[docs.currentSlide].backgroundAssetId || ""
            }
            backgroundCombo.currentIndex = 0
            for (var j = 0; j < docs.assets.length; ++j) {
                if (docs.assets[j].id === background) {
                    backgroundCombo.currentIndex = j + 1
                    break
                }
            }
            // The fit is the slide's, spelled by the model; an
            // unresolvable value leaves the combo where it is rather
            // than pretending to select a name that does not exist.
            var fit = docs.currentBackgroundFit
            fitCombo.currentIndex = fit === "blur" ? 2 : (fit === "contain" ? 1 : 0)
        }
        Component.onCompleted: sync()
        Connections {
            target: docs
            function onDocumentsChanged() { stylePanel.sync() }
            function onNavigationChanged() { stylePanel.sync() }
        }
        Connections {
            target: editPage
            function onVisibleChanged() { if (editPage.visible) stylePanel.sync() }
        }

        Text {
            color: "#8f93a5"
            font.pixelSize: 10
            text: "STYLE"
        }

        ComboBox {
            id: styleCombo
            objectName: "styleCombo"
            width: parent.width
            textRole: "name"
            model: [{ id: "", name: "— show default —" }].concat(docs.styles)
            // Choosing a style overrides the current slide with it;
            // the first row (empty id) hands the slide back.
            onActivated: function (index) {
                if (!docs.applyStyleToCurrentSlide(stylePanel.styleIdAt(index))) {
                    stylePanel.sync()
                }
            }
        }

        Row {
            spacing: 6
            PanelButton {
                objectName: "styleNewButton"
                text: "New"
                // Forks what the slide draws now for this slide only.
                onClicked: docs.createStyleFromCurrent(
                               fieldStyleName.text.trim() || "Style")
            }
            PanelButton {
                objectName: "styleDefaultButton"
                text: "Show default"
                enabled: stylePanel.targetId() !== ""
                onClicked: docs.setShowDefaultStyle(stylePanel.targetId())
            }
            PanelButton {
                objectName: "styleFollowButton"
                text: "Follow show"
                enabled: docs.currentStyleIsOverride
                onClicked: docs.applyStyleToCurrentSlide("")
            }
        }

        PanelField {
            id: fieldStyleName
            objectName: "styleNameField"
            width: parent.width
            onEditingFinished: stylePanel.commit()
        }

        Row {
            spacing: 8
            Column {
                spacing: 2
                Text {
                    color: "#8f93a5"
                    font.pixelSize: 10
                    text: "TEXT COLOUR"
                }
                PanelField {
                    id: fieldTextColor
                    objectName: "styleColourField"
                    width: 116
                    onEditingFinished: stylePanel.commit()
                }
            }
            Column {
                spacing: 2
                Text {
                    color: "#8f93a5"
                    font.pixelSize: 10
                    text: "BACKGROUND"
                }
                PanelField {
                    id: fieldBackColour
                    objectName: "styleBackField"
                    width: 116
                    onEditingFinished: stylePanel.commit()
                }
            }
        }

        // The background's second colour (task 6.17c3): empty keeps
        // the flat fill above; a #rgb paints the gradient's bottom.
        // Its own full-width row — the two colour fields above fill
        // the panel between them.
        Column {
            spacing: 2
            width: stylePanel.width
            Text {
                color: "#8f93a5"
                font.pixelSize: 10
                text: "BACKGROUND GRADIENT (EMPTY = FLAT)"
            }
            PanelField {
                id: fieldBackGradient
                objectName: "styleBackGradientField"
                width: parent.width
                onEditingFinished: stylePanel.commit()
            }
        }

        Row {
            spacing: 8
            Column {
                spacing: 2
                Text {
                    color: "#8f93a5"
                    font.pixelSize: 10
                    text: "FONT"
                }
                PanelField {
                    id: fieldFontFamily
                    objectName: "styleFontField"
                    width: 156
                    onEditingFinished: stylePanel.commit()
                }
            }
            Column {
                spacing: 2
                Text {
                    color: "#8f93a5"
                    font.pixelSize: 10
                    text: "SIZE"
                }
                PanelField {
                    id: fieldStyleSize
                    objectName: "styleSizeField"
                    width: 60
                    onEditingFinished: stylePanel.commit()
                }
            }
        }

        Row {
            spacing: 8
            Row {
                spacing: 4
                Repeater {
                    model: [
                        { key: "left", label: "L" },
                        { key: "center", label: "C" },
                        { key: "right", label: "R" },
                    ]
                    delegate: PanelButton {
                        required property var modelData
                        objectName: "styleAlign" + modelData.key
                        text: modelData.label
                        highlighted: (docs.currentStyle.align || "center") === modelData.key
                        onClicked: stylePanel.commit(modelData.key)
                    }
                }
            }
            Column {
                spacing: 2
                Text {
                    color: "#8f93a5"
                    font.pixelSize: 10
                    text: "LINE SPACING"
                }
                PanelField {
                    id: fieldLineSpacing
                    objectName: "styleSpacingField"
                    width: 60
                    onEditingFinished: stylePanel.commit()
                }
            }
        }

        Text {
            color: "#8f93a5"
            font.pixelSize: 10
            text: "BACKGROUND IMAGE"
        }

        ComboBox {
            id: backgroundCombo
            objectName: "backgroundCombo"
            width: parent.width
            textRole: "name"
            model: [{ id: "", name: "None" }].concat(docs.assets)
            onActivated: function (index) {
                var assetId = ""
                if (index > 0 && index - 1 < docs.assets.length) {
                    assetId = docs.assets[index - 1].id
                }
                if (!docs.setSlideBackground(assetId)) {
                    stylePanel.sync()
                }
            }
        }

        // Cover fills the frame and crops; contain keeps the whole
        // picture and lets the fill show in the bars (6.17c1 — the
        // choice the model stores and the worker honours, one undo
        // step, refused names landing on the status line); blur puts
        // the contained picture over a blurred cover backdrop of
        // itself at the reference's 0.3 (6.17c2 — refused on a video
        // background, and the sync below snaps the combo back).
        Text {
            color: "#8f93a5"
            font.pixelSize: 10
            text: "BACKGROUND FIT"
        }

        ComboBox {
            id: fitCombo
            objectName: "backgroundFitCombo"
            width: parent.width
            model: ["Fill and crop (cover)", "Fit whole picture (contain)",
                    "Blurred backdrop (blur)"]
            onActivated: function (index) {
                var name = index === 2 ? "blur" : (index === 1 ? "contain" : "cover")
                if (!docs.setSlideBackgroundFit(name)) {
                    stylePanel.sync()
                }
            }
        }
    }
}
