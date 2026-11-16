import QtQuick
import QtQuick.Controls

/*
 * UI part: TextStyleControls; contract: docs/ui/control-sets.md#textstylecontrols
 *
 * Family, size, alignment and line spacing, plus (for targets that store
 * the style block) weight, slant, fit and minimum size (task CS.5). Every
 * row commits exactly ONE field through the adapter, so a change here
 * never disturbs the others and never costs more than one undo.
 *
 * The rail reads the target's own state VISUALLY (review 2026-11-10): a
 * small dot beside each style-block label is accent-coloured while the
 * target speaks, and the ↺ chip stands up exactly when there is an own
 * value to hand back — the same badge tradition the Edit page's fields
 * carry, in place of the old "own/inherited" word plus an Inherit button
 * on every row. Small enumerations are segmented choices (`SegmentedControl`)
 * so the active alignment/slant/fit is visible without opening a list; the
 * nine weights stay a picker because nine segments do not fit a rail.
 *
 * Behaviour extracted verbatim from ThemeEditorPage (tasks TS.4/TF.4): the
 * Songs rail edits the theme's base fields and style block; the Bible rail
 * edits the SPARSE reading context, where an empty field is silence that
 * falls through to the theme — a sparse row shows the own value or empty,
 * never the resolved fallback, and an empty typed commit inherits. Where a
 * target cannot store a field (a text context until TV.3) the adapter says
 * so, and the row disables instead of writing another target's record.
 */
Column {
    id: style

    required property InspectorAdapter adapter
    required property var worker           // for the embedded family row
    // A sparse target (a text context): "" commits mean silence, the size
    // may be 0 (the renderer's own default), and the family row is typed
    // with no Browse.
    property bool sparse: false
    // Sparse storage can still present the resolved values, with reset chips
    // returning overrides to inheritance instead of blanking the controls.
    property bool showInheritedValues: false
    // The style block belongs to themes and slides; a text context carries
    // none (TV.3), so its rail hides these rows.
    property bool showStyleBlock: true
    // Optional heading above the style-block rows ("Typography & effects").
    property string sectionTitle: ""

    // The family and size share one row (review 2026-11-10): the typeface
    // name opens the worker's list, the size sits where Browse used to be,
    // and neither row needs a label. Strings stay configurable for hosts
    // that want their own heading, but the default is silent.
    property string sizeLabel: ""
    property string alignLabel: "Alignment"
    property string spacingLabel: "Line spacing (0.25 to 8)"
    property string fontLabel: ""
    property bool fontShowStatus: true
    property Item overlayParent: null
    // The Edit page's badge tradition (see BackgroundControls): a ↺ stands
    // beside each row this target can hand back up the chain, named
    // <prefix><Field>. The style-block rows wear the same chip, named by
    // their own pinned *InheritObjectName below.
    property bool showRevertChips: false
    property string revertBadgePrefix: "customizeBadge"

    property string fontObjectName: "fontFamilyField"
    property string fontStatusObjectName: "fontStatus"
    property string fontPopupObjectName: "fontPopup"
    property string fontSearchObjectName: "fontSearch"
    property string fontListStatusObjectName: "fontListStatus"
    property string fontListObjectName: "fontList"
    property string fontRowObjectName: "fontRow"
    property string sizeObjectName: "fontSizeField"
    property string alignObjectName: "alignCombo"
    property string verticalAlignObjectName: "verticalAlignCombo"
    property string verticalAlignInheritObjectName: "verticalAlignInherit"
    property string allCapsObjectName: "allCapsToggle"
    property string allCapsInheritObjectName: "allCapsInherit"
    property string spacingObjectName: "lineSpacingField"
    property string weightObjectName: "weightToggle"
    property string weightInheritObjectName: "weightInherit"
    property string slantObjectName: "slantToggle"
    property string slantInheritObjectName: "slantInherit"
    property string underlineObjectName: "underlineToggle"
    property string underlineInheritObjectName: "underlineInherit"
    property string fitObjectName: "fitCombo"
    property string fitInheritObjectName: "fitInherit"
    property string minSizeObjectName: "minSizeField"
    property string minSizeInheritObjectName: "minSizeInherit"

    spacing: 8
    width: parent ? parent.width : 0

    // The family row, for hosts that name it or watch its choice.
    readonly property alias fontPicker: fontPicker

    // The adapter's answers are function calls, so every display value is
    // ASSIGNED here (never bound): a control the operator is mid-edit on
    // must not be yanked by an unrelated change, and a successful commit
    // re-reads through the model's own emit anyway.
    function state(field) { return style.adapter.fieldState(field) }
    function capitalise(field) {
        return field.charAt(0).toUpperCase() + field.substring(1)
    }
    function sync() {
        fontPicker.sync()
        if (style.sparse && !style.showInheritedValues) {
            sizeField.text = ownOrEmpty(state("fontSize"))
            spacingField.text = ownOrEmpty(state("lineSpacing"))
            alignCombo.currentIndex = state("align").isSet === true
                    ? ["left", "center", "right"].indexOf(state("align").ownValue) + 1 : 0
        } else {
            const size = state("fontSize")
            sizeField.text = size.value === undefined ? "" : String(size.value)
            const spacing = state("lineSpacing")
            spacingField.text = spacing.value === undefined ? "" : String(spacing.value)
            alignCombo.currentIndex = ["left", "center", "right"].indexOf(state("align").value)
        }
        sizeChip.visible = style.showRevertChips && state("fontSize").canReset === true
        sizeChip.enabled = sizeChip.visible
        alignChip.visible = style.showRevertChips && state("align").canReset === true
        alignChip.enabled = alignChip.visible
        // Task TL.5's vertical row and All caps checkbox, in the shared
        // layout section: the resolved choice is shown, the dot says whether
        // this target speaks, and the chip hands the answer back.
        {
            const anchor = state("verticalAlign")
            if (style.sparse && !style.showInheritedValues) {
                verticalCombo.currentIndex = anchor.isSet === true
                        ? ["top", "center", "bottom"].indexOf(anchor.ownValue) + 1 : 0
            } else {
                verticalCombo.currentIndex =
                        ["top", "center", "bottom"].indexOf(anchor.value)
            }
            verticalInherit.visible = style.showRevertChips && anchor.canReset === true
            verticalInherit.enabled = verticalInherit.visible
            verticalDot.dotColor = ownColor("verticalAlign")
            const caps = state("allCaps")
            allCapsToggle.checked = caps.value === true
            allCapsInherit.visible = style.showRevertChips && caps.canReset === true
            allCapsInherit.enabled = allCapsInherit.visible
            allCapsDot.dotColor = ownColor("allCaps")
        }
        spacingChip.visible = style.showRevertChips && state("lineSpacing").canReset === true
        spacingChip.enabled = spacingChip.visible
        if (!style.showStyleBlock)
            return
        // B / I / U: the style block reads as the three switches every
        // word processor has (review 2026-11-10). A resolved weight at or
        // above 600 is the bold state; the contract still stores the full
        // 100..900 scale, the UI just authors its two poles.
        weightToggle.active = Number(style.state("fontWeight").value) >= 600
        slantToggle.active = style.state("fontStyle").value === "italic"
        underlineToggle.active = style.state("fontUnderline").value === true
        fitCombo.currentIndex = style.state("fitMode").value === "fixed" ? 1 : 0
        minSizeField.text = String(style.state("minFontSize").value)
        fitDot.dotColor = ownColor("fitMode")
        minSizeDot.dotColor = ownColor("minFontSize")
        // B / I / U read as three clean glyphs while everything is
        // inherited; a ↺ appears beside the switch this target OWNS, so
        // the row says both what it draws and what it changed. The fit and
        // minimum rows keep their chips in place (their geometry is fixed).
        weightInherit.visible = style.state("fontWeight").canReset === true
        weightInherit.enabled = weightInherit.visible
        slantInherit.visible = style.state("fontStyle").canReset === true
        slantInherit.enabled = slantInherit.visible
        underlineInherit.visible = style.state("fontUnderline").canReset === true
        underlineInherit.enabled = underlineInherit.visible
        fitInherit.enabled = style.state("fitMode").canReset === true
        minSizeInherit.enabled = style.state("minFontSize").canReset === true
    }
    function ownOrEmpty(s) { return s.isSet === true ? String(s.ownValue) : "" }
    function ownColor(field) {
        return style.state(field).isSet === true ? "#8ec9f5" : "#777777"
    }
    function ownStateText(field) {
        const s = style.state(field)
        if (s.isSet === true)
            return "this target sets its own " + field + "; click the arrow to inherit"
        return "inherited — " + (s.source !== undefined && s.source !== ""
                                 ? s.source + " answers" : "the renderer default answers")
    }
    // A commit that the model turns down has no documentsChanged to
    // repaint from — the controls go back to the record here.
    function commitOrSync(field, value) {
        if (!style.adapter.commit(field, value))
            style.sync()
    }
    // A focus-out with nothing changed is not an edit (see
    // BackgroundControls.commitIfChanged): typed text equal to the record
    // re-reads and writes nothing. Explicit choices still commit whole —
    // a materialise by a deliberate pick is kept.
    function commitNumberIfChanged(field, textIn) {
        if (textIn.trim() === String(style.state(field).value)) {
            style.sync()
            return
        }
        style.commitOrSync(field, Number(textIn))
    }
    function resetOrSync(field) {
        if (!style.adapter.reset(field))
            style.sync()
    }
    // Sparse numbers: "" is silence, and a value out of the row's shape
    // never reaches the model — the field snaps back to the record.
    function commitSparseNumber(field, textIn, lo, hi) {
        const t = textIn.trim()
        if (t === "") {
            resetOrSync(field)
            return
        }
        const v = Number(t)
        if (!isFinite(v) || (lo !== undefined && v < lo) || (hi !== undefined && v > hi)) {
            style.sync()
            return
        }
        const s = style.state(field)
        if ((style.showInheritedValues && v === Number(s.value))
                || (s.isSet === true && v === Number(s.ownValue))) {
            style.sync()  // a focus-out restating the own value is no edit
            return
        }
        commitOrSync(field, v)
    }

    Connections {
        target: style.adapter
        function onChanged() { style.sync() }
        function onTargetChanged() { style.sync() }
    }
    Component.onCompleted: style.sync()

    // The Edit page's RevertBadge, extracted (see BackgroundControls). The
    // style-block rows pin their own names (weightInheritObjectName and
    // friends), so the chip accepts a preferred identifier.
    component RevertChip: Rectangle {
        id: chip
        required property string chipField
        property string preferredName: ""
        property string stateSentence: ""
        objectName: chip.preferredName !== ""
                   ? chip.preferredName
                   : style.revertBadgePrefix + style.capitalise(chip.chipField)
        Accessible.name: chip.stateSentence !== ""
                         ? chip.stateSentence
                         : "Revert " + chip.chipField + " customization"
        width: 24
        height: 30
        radius: 4
        // The size/align/spacing chips still appear only when they can
        // act (the Edit rail's tradition); sync() assigns that. The
        // style-block chips stay put and merely dim (see sync).
        color: chipArea.containsMouse ? "#565659" : "#3a3a3c"
        border.color: chipArea.containsMouse ? "#8a8a8d" : "#6a6a6d"
        Text {
            anchors.centerIn: parent
            color: chip.enabled ? "#ffd666" : "#777777"
            font.pixelSize: 13
            text: "\u21ba"
        }
        MouseArea {
            id: chipArea
            anchors.fill: parent
            hoverEnabled: true
            enabled: chip.enabled
            cursorShape: Qt.PointingHandCursor
            ToolTip.visible: containsMouse && chip.enabled
            ToolTip.text: chip.stateSentence !== "" ? chip.stateSentence
                          : "Hand " + chip.chipField + " back up the chain"
            onClicked: style.resetOrSync(chip.chipField)
        }
    }

    // The visual own/inherited state: a dot that reads at a glance, with
    // its sentence in Accessible.name and a tooltip on hover.
    component StateDot: Item {
        id: dot
        required property string dotField
        property color dotColor: "#777777"
        width: 9
        height: 13   // the label text band, so the dot sits on its baseline
        Accessible.name: style.ownStateText(dot.dotField)
        Rectangle {
            anchors.centerIn: parent
            width: 6
            height: 6
            radius: 3
            color: dot.dotColor
        }
        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            ToolTip.visible: containsMouse
            ToolTip.text: style.ownStateText(dot.dotField)
        }
    }

    Text {
        visible: style.fontLabel !== ""
        text: style.fontLabel
        color: "#b0b0b0"
        font.pixelSize: 11
    }
    Row {
        width: parent.width
        spacing: 4
        WorkerFontPicker {
            id: fontPicker
            width: parent.width - sizeField.width - parent.spacing
                     - (sizeChip.visible ? sizeChip.width + parent.spacing : 0)
            worker: style.worker
            adapter: style.adapter
            field: "fontFamily"
            sparse: style.sparse
            showInheritedValues: style.showInheritedValues
            label: ""
            showStatus: style.fontShowStatus
            showRevertChip: style.showRevertChips
            revertBadgePrefix: style.revertBadgePrefix
            overlayParent: style.overlayParent
            familyObjectName: style.fontObjectName
            statusObjectName: style.fontStatusObjectName
            popupObjectName: style.fontPopupObjectName
            searchObjectName: style.fontSearchObjectName
            listStatusObjectName: style.fontListStatusObjectName
            listObjectName: style.fontListObjectName
            rowObjectName: style.fontRowObjectName
        }
        PanelField {
            id: sizeField
            objectName: style.sizeObjectName
            width: 62
            onEditingFinished: {
                if (style.sparse)
                    style.commitSparseNumber("fontSize", text, 0, undefined)
                else
                    style.commitNumberIfChanged("fontSize", text)
            }
        }
        RevertChip { id: sizeChip; chipField: "fontSize" }
    }
    Text { text: style.alignLabel; color: "#b0b0b0"; font.pixelSize: 11 }
    Row {
        spacing: 2
        SegmentedControl {
            id: alignCombo
            objectName: style.alignObjectName
            width: style.width - (alignChip.visible ? 26 : 0)
            glyphStyle: "align"
            model: style.sparse && !style.showInheritedValues ? ["(theme)", "left", "center", "right"]
                                 : ["left", "center", "right"]
            onActivated: function (index) {
                if (style.sparse && !style.showInheritedValues) {
                    // "(theme)" is the sparse target's silence.
                    if (index === 0)
                        style.resetOrSync("align")
                    else
                        style.commitOrSync("align", currentValue)
                } else {
                    style.commitOrSync("align", currentValue)
                }
            }
        }
        RevertChip { id: alignChip; chipField: "align" }
    }
    // Task TL.5: the vertical row sits beside the horizontal alignment row,
    // in the same visual language — three in-repository paragraph marks
    // (top/centre/bottom), reachable by Tab and activated by Space/Return.
    Row {
        spacing: 4
        Text { text: "Vertical"; color: "#b0b0b0"; font.pixelSize: 11 }
        StateDot { id: verticalDot; dotField: "verticalAlign" }
    }
    Row {
        spacing: 2
        SegmentedControl {
            id: verticalCombo
            objectName: style.verticalAlignObjectName
            width: style.width - (verticalInherit.visible ? 26 : 0)
            glyphStyle: "vertical"
            model: style.sparse && !style.showInheritedValues
                   ? [{value: "(theme)", label: "Theme"},
                      {value: "top", label: "Top"},
                      {value: "center", label: "Centre"},
                      {value: "bottom", label: "Bottom"}]
                   : [{value: "top", label: "Top"},
                      {value: "center", label: "Centre"},
                      {value: "bottom", label: "Bottom"}]
            onActivated: function (index) {
                if (style.sparse && !style.showInheritedValues && index === 0) {
                    style.resetOrSync("verticalAlign")
                } else {
                    style.commitOrSync("verticalAlign", currentValue)
                }
            }
        }
        RevertChip {
            id: verticalInherit
            chipField: "verticalAlign"
            preferredName: style.verticalAlignInheritObjectName
        }
    }
    Row {
        spacing: 4
        CheckBox {
            id: allCapsToggle
            objectName: style.allCapsObjectName
            text: "All caps"
            padding: 0
            spacing: 4
            font.pixelSize: 11
            Accessible.name: checked ? "All caps on" : "All caps off"
            // clicked only: sync() assigns checked from the record and must
            // never commit the value it just read back.
            onClicked: style.commitOrSync("allCaps", checked)
            indicator: Rectangle {
                implicitWidth: 14
                implicitHeight: 14
                x: allCapsToggle.leftPadding
                y: parent.height / 2 - height / 2
                radius: 3
                color: allCapsToggle.checked ? "#1685f8" : "#2b2b2c"
                border.color: allCapsToggle.checked ? "#1685f8" : "#6a6a6d"
                Rectangle {
                    visible: allCapsToggle.checked
                    width: 4
                    height: 7
                    x: 5
                    y: 2
                    rotation: 45
                    color: "#ffffff"
                }
            }
            contentItem: Text {
                text: allCapsToggle.text
                color: "#cccccc"
                font.pixelSize: 11
                leftPadding: allCapsToggle.indicator.width + allCapsToggle.spacing
                verticalAlignment: Text.AlignVCenter
            }
        }
        StateDot { id: allCapsDot; dotField: "allCaps" }
        RevertChip {
            id: allCapsInherit
            chipField: "allCaps"
            preferredName: style.allCapsInheritObjectName
        }
    }
    Text { text: style.spacingLabel; color: "#b0b0b0"; font.pixelSize: 11 }
    Row {
        spacing: 2
        PanelField {
            id: spacingField
            objectName: style.spacingObjectName
            width: style.width - (spacingChip.visible ? 26 : 0)
            onEditingFinished: {
                if (style.sparse)
                    style.commitSparseNumber("lineSpacing", text, 0.25, 8)
                else
                    style.commitNumberIfChanged("lineSpacing", text)
            }
        }
        RevertChip { id: spacingChip; chipField: "lineSpacing" }
    }

    Column {
        visible: style.showStyleBlock
        width: parent.width
        spacing: 8
        Text {
            visible: style.sectionTitle !== ""
            text: style.sectionTitle
            color: "#bbbbbb"
            font.pixelSize: 11
            font.bold: true
        }

        // B / I / U, one switch each, with the same ↺ inheritance chip the
        // other style rows carry. The glyphs wear their own treatment so
        // the icon IS the sample: bold B, italic I, underlined U.
        Row {
            spacing: 4
            PanelButton {
                id: weightToggle
                objectName: style.weightObjectName
                property bool active: false
                width: 32
                text: "B"
                font.bold: true
                font.pixelSize: 15
                highlighted: active
                Accessible.name: active ? "Turn bold off" : "Turn bold on"
                onClicked: style.commitOrSync("fontWeight", active ? 400 : 700)
            }
            RevertChip {
                id: weightInherit
                chipField: "fontWeight"
                preferredName: style.weightInheritObjectName
            }
            PanelButton {
                id: slantToggle
                objectName: style.slantObjectName
                property bool active: false
                width: 32
                text: "I"
                font.italic: true
                font.pixelSize: 15
                highlighted: active
                Accessible.name: active ? "Turn italic off" : "Turn italic on"
                onClicked: style.commitOrSync("fontStyle", active ? "normal" : "italic")
            }
            RevertChip {
                id: slantInherit
                chipField: "fontStyle"
                preferredName: style.slantInheritObjectName
            }
            PanelButton {
                id: underlineToggle
                objectName: style.underlineObjectName
                property bool active: false
                width: 32
                text: "U"
                font.underline: true
                font.pixelSize: 15
                highlighted: active
                Accessible.name: active ? "Turn underline off" : "Turn underline on"
                onClicked: style.commitOrSync("fontUnderline", !active)
            }
            RevertChip {
                id: underlineInherit
                chipField: "fontUnderline"
                preferredName: style.underlineInheritObjectName
            }
        }

        Row {
            spacing: 4
            Text { text: "Fit"; color: "#b0b0b0"; font.pixelSize: 11 }
            StateDot { id: fitDot; dotField: "fitMode" }
        }
        Row {
            spacing: 6
            SegmentedControl {
                id: fitCombo
                objectName: style.fitObjectName
                width: style.width - (fitInherit.visible ? 30 : 0)
                model: ["shrink", "fixed"]
                onActivated: style.commitOrSync("fitMode", currentValue)
            }
            RevertChip {
                id: fitInherit
                chipField: "fitMode"
                preferredName: style.fitInheritObjectName
            }
        }

        Row {
            spacing: 4
            Text { text: "Minimum size (1..4096)"; color: "#b0b0b0"; font.pixelSize: 11 }
            StateDot { id: minSizeDot; dotField: "minFontSize" }
        }
        Row {
            width: parent.width
            spacing: 2
            PanelField {
                id: minSizeField
                objectName: style.minSizeObjectName
                width: parent.width - (minSizeInherit.visible ? 26 : 0)
                onEditingFinished: {
                    if (text.trim() === String(style.state("minFontSize").value))
                        return  // focus-out with no change writes nothing
                    const v = Number(text)
                    if (isFinite(v) && v >= 1 && v <= 4096)
                        style.commitOrSync("minFontSize", v)
                    else
                        style.sync()
                }
            }
            RevertChip {
                id: minSizeInherit
                chipField: "minFontSize"
                preferredName: style.minSizeInheritObjectName
            }
        }
    }
}
