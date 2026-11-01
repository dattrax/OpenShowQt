import QtQuick
import QtQuick.Controls

/*
 * The edit page (task 4.3, stack since 6.17a2; task 6.19b7 out of
 * Main.qml): the current slide's boxes on a 16:9 stage — click selects
 * the topmost box, drag moves it, corners resize it, Tab walks the
 * stack, arrows nudge — with exact X/Y/W/H fields and
 * New/Delete/Back/Forward under it as the deterministic path, and the
 * theme panel beside it. One undo entry per drag, per nudge, per field
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
 * theme editor, all addressing `docs` and `worker` directly.
 */
Flickable {
    id: editPage
    contentWidth: Math.max(width, 760)
    contentHeight: Math.max(height, 650)
    clip: true
    ScrollBar.horizontal: ScrollBar {}
    ScrollBar.vertical: ScrollBar {}

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


    function askForWorkerPicture() { selectionAsk.restart() }

    Flow {
        id: slideActions
        anchors.top: parent.top; anchors.left: parent.left
        anchors.right: themeScroll.left; anchors.rightMargin: 12
        spacing: 6
        ComboBox {
            width: Math.min(180, slideActions.width)
            height: 30
            model: docs.slides.map(s => ({label: (s.index + 1) + ". " + s.preview}))
            textRole: "label"
            currentIndex: docs.currentSlide
            onActivated: docs.selectSlide(currentIndex)
        }
        PanelButton { text: "Add slide"; enabled: docs.currentSongId !== ""; onClicked: docs.addSlide() }
        PanelButton { text: "Remove slide"; enabled: docs.slides.length > 0; onClicked: docs.removeCurrentSlide() }
        PanelButton { text: "Undo"; enabled: docs.canUndo; onClicked: docs.undo() }
        PanelButton { text: "Redo"; enabled: docs.canRedo; onClicked: docs.redo() }
        PanelButton { text: "Save"; enabled: docs.dirty; onClicked: docs.save() }
        PanelButton { text: "Load"; onClicked: docs.load() }
    }

    ScrollView {
        id: lyricEditor
        anchors.left: parent.left; anchors.right: themeScroll.left
        anchors.rightMargin: 12
        anchors.bottom: boxFields.top; anchors.bottomMargin: 12
        height: 110
        clip: true
        TextArea {
            id: lyricText
            objectName: "slideTextEditor"
            enabled: docs.slides.length > 0 && docs.currentBoxKind !== "image"
            text: docs.currentSlideText
            placeholderText: docs.currentBoxKind === "image" ? "Select a text box to edit its words" : "Slide text"
            onTextChanged: if (activeFocus && enabled) docs.setSlideText(text)
            selectByMouse: true
            wrapMode: TextArea.Wrap
            color: "#eeeeee"
            font.pixelSize: 15
            background: Rectangle { color: "#242424"; border.color: "#4a4a4a" }
            Connections {
                target: docs
                function onSlideTextChanged() {
                    lyricText.text = Qt.binding(function() { return docs.currentSlideText })
                }
            }
        }
    }


    EditCanvas {
        id: editCanvas
        anchors.top: slideActions.bottom
        anchors.topMargin: 12
        anchors.left: parent.left
        anchors.right: themeScroll.left
        anchors.rightMargin: 12
        anchors.bottom: lyricEditor.top
        anchors.bottomMargin: 12
    }

    /*
     * Asking the worker for its picture of the slide under edit (task
     * 6.16a, review R16). Debounced: one ask costs a scene.prepare and an
     * off-air draw, so a keystroke at a time would be renders nobody has
     * looked at yet. The triggers below are everything that can change
     * what the canvas should show — text, theme, geometry (a drag
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

    Flow {
        id: boxFields
        anchors.right: themeScroll.left
        anchors.rightMargin: 12
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
                color: "#b0b0b0"
                font.pixelSize: 10
                text: "X"
            }
            TextField {
                id: fieldX
                objectName: "geometryField"
                width: 60
                color: "#eeeeee"
                font.pixelSize: 13
                onEditingFinished: boxFields.commit()
                background: Rectangle {
                    color: "#242424"
                    border.color: "#4a4a4a"
                }
            }
        }

        Column {
            spacing: 2
            Text {
                color: "#b0b0b0"
                font.pixelSize: 10
                text: "Y"
            }
            TextField {
                id: fieldY
                objectName: "geometryField"
                width: 60
                color: "#eeeeee"
                font.pixelSize: 13
                onEditingFinished: boxFields.commit()
                background: Rectangle {
                    color: "#242424"
                    border.color: "#4a4a4a"
                }
            }
        }

        Column {
            spacing: 2
            Text {
                color: "#b0b0b0"
                font.pixelSize: 10
                text: "W"
            }
            TextField {
                id: fieldW
                objectName: "geometryField"
                width: 60
                color: "#eeeeee"
                font.pixelSize: 13
                onEditingFinished: boxFields.commit()
                background: Rectangle {
                    color: "#242424"
                    border.color: "#4a4a4a"
                }
            }
        }

        Column {
            spacing: 2
            Text {
                color: "#b0b0b0"
                font.pixelSize: 10
                text: "H"
            }
            TextField {
                id: fieldH
                objectName: "geometryField"
                width: 60
                color: "#eeeeee"
                font.pixelSize: 13
                onEditingFinished: boxFields.commit()
                background: Rectangle {
                    color: "#242424"
                    border.color: "#4a4a4a"
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
                color: "#b0b0b0"
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


    }

    /*
     * The theme panel (tasks 4.4, TH.5): the fields show the RESOLVED
     * appearance of the current slide — customization over the current
     * theme over the slide's own theme over the show default over the
     * built-in look. An edit here customizes THIS SLIDE only: the shared
     * theme entity is edited in the Theme Editor alone, so restyling one
     * lyric can never repaint every other slide wearing the same theme
     * — the footgun the themes plan names and removes. A customized
     * field carries a revert badge, and "Remove customizations" sends
     * the whole slide back to its themes in one undo step. Refusals land
     * on the status line and the field snaps back, like the box fields
     * beside them.
     */
    ScrollView {
        id: themeScroll
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        width: 250
        contentWidth: availableWidth
        clip: true
    Column {
        id: themePanel
        objectName: "themePanel"
        width: themeScroll.availableWidth
        spacing: 10

        // The appearance fields in model spelling: the badge rules, the
        // restore button's enabled state and the sync below all read it.
        readonly property var fieldNames: ["textColor", "backgroundColor",
                                           "backgroundGradient", "fontFamily",
                                           "fontSize", "align", "lineSpacing"]

        // One resolved value with the built-in look behind it, so the
        // panel is never blank between slides or before any theme exists.
        function valueOf(field, fallback) {
            const entry = docs.slideAppearance[field]
            if (entry === undefined || entry.value === undefined) {
                return fallback
            }
            return entry.value
        }
        function isCustomized(field) {
            const entry = docs.slideAppearance[field]
            return entry !== undefined && entry.customized === true
        }
        function customizedAny() {
            for (var i = 0; i < fieldNames.length; ++i) {
                if (isCustomized(fieldNames[i])) {
                    return true
                }
            }
            return false
        }
        // Focus-out commit: ONE customization for ONE field, never a
        // write to the shared entity. The domain vets the value by the
        // theme's own contract — "" is a real answer for the gradient
        // (explicitly flat, 6.17c3) and the family (whatever the
        // platform picks) — and a bad value snaps the field back.
        // editingFinished also fires on a bare focus-out, and leaving a
        // field untouched is NOT an edit: a value the chain already
        // resolves to customizes nothing and just puts the binding back.
        function commit(field, value) {
            const entry = docs.slideAppearance[field]
            if (entry !== undefined && entry.value === value) {
                themePanel.sync()
                return
            }
            if (!docs.customizeCurrentSlide(field, value)) {
                themePanel.sync()
            }
        }
        // Combo row 0 is "follow show" (the empty id); the rest name
        // the themes view's rows in order.
        function themeIdAt(index) {
            if (index <= 0) {
                return ""
            }
            var i = index - 1
            return i < docs.themes.length ? docs.themes[i].id : ""
        }
        // The theme the slide wears through the chain ("" = nowhere).
        function targetId() {
            return docs.currentTheme.id || ""
        }
        // Re-derive every field from the model. Editing a field broke
        // its binding; this is what puts the binding back, so the next
        // navigation or document change repaints the panel again.
        function sync() {
            fieldTextColor.text = Qt.binding(function () {
                return themePanel.valueOf("textColor", "#ffffff")
            })
            fieldBackColour.text = Qt.binding(function () {
                return themePanel.valueOf("backgroundColor", "#000000")
            })
            // Flat shows empty, not a colour: the field IS the "" a
            // customization stores (6.17c3).
            fieldBackGradient.text = Qt.binding(function () {
                return themePanel.valueOf("backgroundGradient", "")
            })
            fieldFontFamily.text = Qt.binding(function () {
                return themePanel.valueOf("fontFamily", "")
            })
            fieldThemeSize.text = Qt.binding(function () {
                return String(themePanel.valueOf("fontSize", 0))
            })
            fieldLineSpacing.text = Qt.binding(function () {
                return String(themePanel.valueOf("lineSpacing", 1.25))
            })
            // The combo names the followed default as its first row;
            // an override selects its own theme instead.
            var index = 0
            if (docs.currentThemeIsOverride) {
                for (var i = 0; i < docs.themes.length; ++i) {
                    if (docs.themes[i].id === themePanel.targetId()) {
                        index = i + 1
                        break
                    }
                }
            }
            themeCombo.currentIndex = index
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
            function onDocumentsChanged() { themePanel.sync() }
            function onNavigationChanged() { themePanel.sync() }
        }
        Connections {
            target: editPage
            function onVisibleChanged() { if (editPage.visible) themePanel.sync() }
        }

        // A customized field's badge: only up while that property is
        // customized, and one click puts it back to the themes. A revert
        // with nothing under it is a wrong click, refused on the status
        // line — the badge's own visibility normally prevents that.
        component RevertBadge: Rectangle {
            id: badge
            required property string field
            objectName: "customizeBadge" + field.charAt(0).toUpperCase()
                        + field.substring(1)
            Accessible.name: "Revert " + field + " customization"
            width: 24
            height: 30 // the PanelField's height, so a Row lines them up
            radius: 4
            visible: themePanel.isCustomized(field)
            color: badgeArea.containsMouse ? "#565659" : "#3a3a3c"
            border.color: "#6a6a6d"
            Text {
                anchors.centerIn: parent
                color: "#ffd666"
                font.pixelSize: 13
                text: "\u21ba"
            }
            MouseArea {
                id: badgeArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    if (!docs.revertCurrentSlideField(badge.field))
                        themePanel.sync()
                }
            }
        }

        Text {
            color: "#b0b0b0"
            font.pixelSize: 10
            text: "THEME"
        }

        ComboBox {
            id: themeCombo
            objectName: "themeCombo"
            width: parent.width
            textRole: "name"
            model: [{ id: "", name: "— show default —" }].concat(docs.themes)
            // Choosing a theme overrides the current slide with it;
            // the first row (empty id) hands the slide back.
            onActivated: function (index) {
                if (!docs.applyThemeToCurrentSlide(themePanel.themeIdAt(index))) {
                    themePanel.sync()
                }
            }
        }

        Row {
            spacing: 6
            PanelButton {
                objectName: "themeNewButton"
                text: "New"
                // Forks what the slide draws now — customizations and all —
                // into a fresh theme worn by this slide alone.
                onClicked: docs.createThemeFromCurrent(
                               (docs.currentTheme.name || "Theme") + " fork")
            }
            PanelButton {
                objectName: "themeDefaultButton"
                text: "Show default"
                enabled: themePanel.targetId() !== ""
                onClicked: docs.setShowDefaultTheme(themePanel.targetId())
            }
            PanelButton {
                objectName: "themeFollowButton"
                text: "Follow show"
                enabled: docs.currentThemeIsOverride
                onClicked: docs.applyThemeToCurrentSlide("")
            }
        }

        // The theme this slide wears through the chain — a name the
        // operator READS here. The entity itself lives in the Theme
        // Editor; editing it from this page would restyle every slide
        // wearing it, which is exactly what TH.5 took away.
        Text {
            id: fieldThemeName
            objectName: "themeWornName"
            width: parent.width
            color: "#e6e6e6"
            font.pixelSize: 12
            elide: Text.ElideRight
            text: docs.currentTheme.name || "Built-in look"
        }

        Row {
            spacing: 8
            Column {
                spacing: 2
                Text {
                    color: "#b0b0b0"
                    font.pixelSize: 10
                    text: "TEXT COLOUR"
                }
                Row {
                    spacing: 2
                    PanelField {
                        id: fieldTextColor
                        objectName: "themeColourField"
                        width: 94
                        onEditingFinished: themePanel.commit("textColor",
                                                             text.trim())
                    }
                    RevertBadge { field: "textColor" }
                }
            }
            Column {
                spacing: 2
                Text {
                    color: "#b0b0b0"
                    font.pixelSize: 10
                    text: "BACKGROUND"
                }
                Row {
                    spacing: 2
                    PanelField {
                        id: fieldBackColour
                        objectName: "themeBackField"
                        width: 94
                        onEditingFinished: themePanel.commit("backgroundColor",
                                                             text.trim())
                    }
                    RevertBadge { field: "backgroundColor" }
                }
            }
        }

        // The background's second colour (task 6.17c3): empty keeps
        // the flat fill above; a #rgb paints the gradient's bottom.
        // Customizing it to "" is the slide saying "flat, whatever the
        // theme grades" — the answer-empty the model stores.
        Column {
            spacing: 2
            width: themePanel.width
            Text {
                color: "#b0b0b0"
                font.pixelSize: 10
                text: "BACKGROUND GRADIENT (EMPTY = FLAT)"
            }
            Row {
                spacing: 2
                PanelField {
                    id: fieldBackGradient
                    objectName: "themeBackGradientField"
                    width: themePanel.width - 26
                    onEditingFinished: themePanel.commit("backgroundGradient",
                                                         text.trim())
                }
                RevertBadge { field: "backgroundGradient" }
            }
        }

        Row {
            spacing: 8
            Column {
                spacing: 2
                Text {
                    color: "#b0b0b0"
                    font.pixelSize: 10
                    text: "FONT"
                }
                Row {
                    spacing: 2
                    PanelField {
                        id: fieldFontFamily
                        objectName: "themeFontField"
                        width: 122
                        // "" is an answer: no family, whatever the
                        // renderer picks.
                        onEditingFinished: themePanel.commit("fontFamily",
                                                             text.trim())
                    }
                    RevertBadge { field: "fontFamily" }
                }
            }
            Column {
                spacing: 2
                Text {
                    color: "#b0b0b0"
                    font.pixelSize: 10
                    text: "SIZE"
                }
                Row {
                    spacing: 2
                    PanelField {
                        id: fieldThemeSize
                        objectName: "themeSizeField"
                        width: 60
                        // 0 is an answer (the renderer's own size);
                        // garbage is not — the field snaps back.
                        onEditingFinished: {
                            const value = parseFloat(text)
                            if (text.trim() === "")
                                themePanel.commit("fontSize", 0)
                            else if (Number.isFinite(value))
                                themePanel.commit("fontSize", value)
                            else
                                themePanel.sync()
                        }
                    }
                    RevertBadge { field: "fontSize" }
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
                        objectName: "themeAlign" + modelData.key
                        text: modelData.label
                        highlighted: String(themePanel.valueOf("align", "center"))
                                     === modelData.key
                        onClicked: themePanel.commit("align", modelData.key)
                    }
                }
            }
            // The align trio needs its own revert too; it wears the same
            // badge as any field, visible only while align is customized.
            RevertBadge { field: "align" }
            Column {
                spacing: 2
                Text {
                    color: "#b0b0b0"
                    font.pixelSize: 10
                    text: "LINE SPACING"
                }
                Row {
                    spacing: 2
                    PanelField {
                        id: fieldLineSpacing
                        objectName: "themeSpacingField"
                        width: 52
                        onEditingFinished: {
                            const value = parseFloat(text)
                            if (Number.isFinite(value))
                                themePanel.commit("lineSpacing", value)
                            else
                                themePanel.sync()
                        }
                    }
                    RevertBadge { field: "lineSpacing" }
                }
            }
        }

        // The whole slide back at its themes in one undo step — the
        // plan's quick restore. It refuses honestly when the slide was
        // never customized, so no empty history entry exists.
        PanelButton {
            objectName: "removeCustomizationsButton"
            text: "Remove customizations"
            enabled: themePanel.customizedAny()
            onClicked: docs.removeCurrentSlideCustomizations()
        }

        Text {
            color: "#b0b0b0"
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
                    themePanel.sync()
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
            color: "#b0b0b0"
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
                    themePanel.sync()
                }
            }
        }
    }
    }
}
