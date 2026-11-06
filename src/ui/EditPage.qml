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

    // The context-property worker, named once at the page root: inside an
    // instance body `worker: worker` resolves to the instance's OWN
    // property and reads undefined (the CS.5 lesson).
    readonly property var workerApi: worker

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
        anchors.top: parent.top
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

        // Where the SELECTED box comes from (amendments 2026-11-01 and
        // 2026-11-02): every geometry write on the slide's PRIMARY lyric
        // box records a placement CUSTOMIZATION — whether a theme places or
        // not — so the owner row and its revert belong to that box always,
        // not only to the ones a theme currently drives.
        readonly property bool primaryBox: {
            const box = editCanvas.boxes[docs.currentBox]
            return box !== undefined && box.primary === true
        }
        readonly property bool placedByTheme: {
            const box = editCanvas.boxes[docs.currentBox]
            return box !== undefined && box.placed === true
        }
        readonly property bool placementCustomized: {
            const entry = docs.slideAppearance["placement"]
            return entry !== undefined && entry.customized === true
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

        // The owner row belongs to the PRIMARY box whatever the chain is
        // doing (amendment 2026-11-02): every write here customizes, so the
        // label says who is drawing the box right now — THIS SLIDE, THEME
        // PLACES, or AUTHORED while nothing speaks.
        Column {
            spacing: 2
            visible: boxFields.primaryBox
            Text {
                objectName: "placementOwner"
                color: boxFields.placementCustomized ? "#ffd666" : "#8ec9f5"
                font.pixelSize: 10
                text: boxFields.placementCustomized ? "THIS SLIDE"
                        : boxFields.placedByTheme ? "THEME PLACES" : "AUTHORED"
            }
            PanelButton {
                objectName: "revertPlacementButton"
                text: "\u21ba Follow theme"
                // Only live while the slide actually holds a placement of
                // its own; the revert refuses out loud either way, so a
                // wrong click costs nothing but says something.
                enabled: boxFields.placementCustomized
                onClicked: docs.revertCurrentSlideField("placement")
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

    // The Edit rail's adapter (CS.4): one read model over the slide the
    // cursor is on — the slide target follows the selection, so the target
    // is a constant and every navigation re-opens the answers through
    // changed(). Refusals, the unchanged-commit rule and the per-field
    // reset all live there; this page adds only the names and the layout.
    InspectorAdapter {
        id: slideAdapter
        document: docs
        target: ({kind: "slide"})
    }

    /*
     * The theme panel (tasks 4.4, TH.5; the appearance rows are the CS.6
     * AppearanceInspector): the fields show the RESOLVED appearance of the
     * current slide — customization over the current theme over the
     * slide's own theme over the show default over the built-in look. An
     * edit here customizes THIS SLIDE only: the shared theme entity is
     * edited in the Theme Editor alone, so restyling one lyric can never
     * repaint every other slide wearing the same theme — the footgun the
     * themes plan names and removes. A customized field grows a revert
     * chip (the parts' badge tradition, same customizeBadge* names as
     * ever), effects inherit whole through their Off button, and "Remove
     * customizations" sends the whole slide back to its themes in one undo
     * step. Refusals land on the status line and the field snaps back,
     * like the box fields beside them. The theme admin row, the background
     * image asset combo and the fit combo stay this page's own — they are
     * slide properties, not appearance fields — and the authored box band
     * at the bottom of the page is where the slide's rectangle lives.
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

        // Everything "Remove customizations" hands back — the base seven,
        // the six fields TS.1 added to a slide's vocabulary (a stroke the
        // slide owns is as much a customization as a colour), and the
        // placement (amendment 2026-11-01). The button must be live when
        // any one of them is set — even when the box's spot is the ONLY
        // thing the slide changed, since that field's revert lives by the
        // geometry fields rather than in this panel.
        readonly property var revertableNames: ["textColor", "backgroundColor",
                                                "backgroundGradient", "fontFamily",
                                                "fontSize", "align", "lineSpacing",
                                                "fontWeight", "fontStyle", "fitMode",
                                                "minFontSize", "stroke", "shadow",
                                                "placement"]
        function isCustomized(field) {
            const entry = docs.slideAppearance[field]
            return entry !== undefined && entry.customized === true
        }
        function customizedAny() {
            for (var i = 0; i < revertableNames.length; ++i) {
                if (isCustomized(revertableNames[i])) {
                    return true
                }
            }
            return false
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
        // The page's own follow-rows re-read from the model: the theme
        // combo names the followed default, the asset combo the slide's
        // picture, and the fit is spelled by the model — an unresolvable
        // value leaves the combo where it is rather than pretending to
        // select a name that does not exist. (The appearance rows used to
        // live here too; since CS.6 the parts re-sync themselves through
        // the adapter, and an unchanged focus-out writes nothing at all.)
        function sync() {
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
            var fit = docs.currentBackgroundFit
            fitCombo.currentIndex = fit === "blur" ? 2 : (fit === "contain" ? 1 : 0)
        }
        Component.onCompleted: {
            themePanel.pinEditNames()
            themePanel.sync()
        }
        Connections {
            target: docs
            function onDocumentsChanged() { themePanel.sync() }
            function onNavigationChanged() { themePanel.sync() }
        }
        Connections {
            target: editPage
            function onVisibleChanged() { if (editPage.visible) themePanel.sync() }
        }

        // The automation names this rail reads. The fields the panel used
        // to own keep their objectNames — the suite (and the operator's
        // muscle memory) finds the same identifiers, served now by the
        // shared parts — and what the parts ADD to this row (the worker
        // font browse and popup, weight, slant, fit, minimum size, stroke
        // and shadow) follow the edit* spelling. Revert chips keep the
        // badge tradition's own names: customizeBadge<Field>, unchanged
        // since before the parts.
        function pinEditNames() {
            const b = editInspector.backgroundControls
            b.textColorObjectName = "themeColourField"
            b.backColorObjectName = "themeBackField"
            b.gradientObjectName = "themeBackGradientField"
            const st = editInspector.styleControls
            st.fontObjectName = "themeFontField"
            st.fontBrowseObjectName = "editBrowseFonts"
            st.fontStatusObjectName = "editFontStatus"
            st.fontPopupObjectName = "editFontPopup"
            st.fontSearchObjectName = "editFontSearch"
            st.fontListStatusObjectName = "editFontListStatus"
            st.fontListObjectName = "editFontList"
            st.fontRowObjectName = "editFontRow"
            st.fontCancelObjectName = "editFontCancel"
            st.sizeObjectName = "themeSizeField"
            st.alignObjectName = "themeAlignCombo"
            st.spacingObjectName = "themeSpacingField"
            st.weightObjectName = "editWeight"
            st.weightInheritObjectName = "editWeightInherit"
            st.slantObjectName = "editSlant"
            st.slantInheritObjectName = "editSlantInherit"
            st.fitObjectName = "editFit"
            st.fitInheritObjectName = "editFitInherit"
            st.minSizeObjectName = "editMinSize"
            st.minSizeInheritObjectName = "editMinSizeInherit"
            const fx = editInspector.effectsControls
            fx.strokeCheckObjectName = "editStrokeEnable"
            fx.strokeOffObjectName = "editStrokeOff"
            fx.strokeColourObjectName = "editStrokeColour"
            fx.strokeWidthObjectName = "editStrokeWidth"
            fx.shadowCheckObjectName = "editShadowEnable"
            fx.shadowOffObjectName = "editShadowOff"
            fx.shadowColourObjectName = "editShadowColour"
            fx.shadowOpacityObjectName = "editShadowOpacity"
            fx.shadowOffsetXObjectName = "editShadowOffsetX"
            fx.shadowOffsetYObjectName = "editShadowOffsetY"
            fx.shadowBlurObjectName = "editShadowBlur"
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

        // The shared inspector (CS.6): the same parts the Songs rail
        // wears, over {kind: "slide"}. A slide answers every appearance
        // field — colours, the whole style block, stroke and shadow — so
        // this rail hides only the two sections its target does not own:
        // the box rectangle lives in the geometry band at the bottom of
        // the page (an authored-item tool with its own placement rules),
        // and a slide never carries its own movement. The revert chips
        // stand where the page's own badges always stood.
        AppearanceInspector {
            id: editInspector
            objectName: "editInspector"
            // The pane's overlay scrollbar floats over the right edge of
            // the content (the dump that found a chip under the bar was
            // the proof); the rows stand clear of the gutter so every
            // chip and control is clickable.
            width: themePanel.width - 18
            adapter: slideAdapter
            worker: editPage.workerApi
            overlayParent: editPage
            showGeometry: false
            showTransition: false
            showRevertChips: true
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
