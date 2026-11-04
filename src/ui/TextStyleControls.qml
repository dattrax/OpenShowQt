import QtQuick
import QtQuick.Controls

/*
 * UI part: TextStyleControls; contract: docs/ui/control-sets.md#textstylecontrols
 *
 * Family, size, alignment and line spacing, plus (for targets that store
 * the style block) weight, slant, fit and minimum size (task CS.5). Every
 * row commits exactly ONE field through the adapter, so a change here
 * never disturbs the others and never costs more than one undo; the badge
 * beside each style field says whether this target speaks or inherits what
 * the audience draws today, and the Inherit button writes that back.
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
    // The style block belongs to themes and slides; a text context carries
    // none (TV.3), so its rail hides these rows.
    property bool showStyleBlock: true
    // Optional heading above the style-block rows ("Typography & effects").
    property string sectionTitle: ""

    property string sizeLabel: "Font size (0 = renderer default)"
    property string alignLabel: "Alignment"
    property string spacingLabel: "Line spacing (0.25 to 8)"
    property string fontLabel: "Font family (\"\" = platform)"
    property bool fontShowStatus: true
    property Item overlayParent: null

    property string fontObjectName: "fontFamilyField"
    property string fontBrowseObjectName: "fontBrowse"
    property string fontStatusObjectName: "fontStatus"
    property string fontPopupObjectName: "fontPopup"
    property string fontSearchObjectName: "fontSearch"
    property string fontListStatusObjectName: "fontListStatus"
    property string fontListObjectName: "fontList"
    property string fontRowObjectName: "fontRow"
    property string fontCancelObjectName: "fontCancel"
    property string sizeObjectName: "fontSizeField"
    property string alignObjectName: "alignCombo"
    property string spacingObjectName: "lineSpacingField"
    property string weightObjectName: "weightCombo"
    property string weightInheritObjectName: "weightInherit"
    property string slantObjectName: "slantCombo"
    property string slantInheritObjectName: "slantInherit"
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
    function sync() {
        fontPicker.sync()
        if (style.sparse) {
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
        if (!style.showStyleBlock)
            return
        const weights = [100, 200, 300, 400, 500, 600, 700, 800, 900]
        const w = weights.indexOf(Number(style.state("fontWeight").value))
        weightCombo.currentIndex = w >= 0 ? w : 3
        slantCombo.currentIndex = style.state("fontStyle").value === "italic" ? 1 : 0
        fitCombo.currentIndex = style.state("fitMode").value === "fixed" ? 1 : 0
        minSizeField.text = String(style.state("minFontSize").value)
        weightBadge.text = ownWord("fontWeight")
        weightBadge.color = ownColor("fontWeight")
        weightInherit.enabled = style.state("fontWeight").canReset === true
        slantInherit.enabled = style.state("fontStyle").canReset === true
        fitInherit.enabled = style.state("fitMode").canReset === true
        minSizeInherit.enabled = style.state("minFontSize").canReset === true
    }
    function ownOrEmpty(s) { return s.isSet === true ? String(s.ownValue) : "" }
    function ownWord(field) {
        return style.state(field).isSet === true ? "own" : "inherited"
    }
    function ownColor(field) {
        return style.state(field).isSet === true ? "#8ec9f5" : "#888888"
    }
    // A commit that the model turns down has no documentsChanged to
    // repaint from — the controls go back to the record here.
    function commitOrSync(field, value) {
        if (!style.adapter.commit(field, value))
            style.sync()
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
        commitOrSync(field, v)
    }

    Connections {
        target: style.adapter
        function onChanged() { style.sync() }
        function onTargetChanged() { style.sync() }
    }
    Component.onCompleted: style.sync()

    WorkerFontPicker {
        id: fontPicker
        width: parent.width
        worker: style.worker
        adapter: style.adapter
        field: "fontFamily"
        sparse: style.sparse
        label: style.fontLabel
        showStatus: style.fontShowStatus
        showBrowse: !style.sparse
        overlayParent: style.overlayParent
        familyObjectName: style.fontObjectName
        browseObjectName: style.fontBrowseObjectName
        statusObjectName: style.fontStatusObjectName
        popupObjectName: style.fontPopupObjectName
        searchObjectName: style.fontSearchObjectName
        listStatusObjectName: style.fontListStatusObjectName
        listObjectName: style.fontListObjectName
        rowObjectName: style.fontRowObjectName
        cancelObjectName: style.fontCancelObjectName
    }
    Text { text: style.sizeLabel; color: "#b0b0b0"; font.pixelSize: 11 }
    PanelField {
        id: sizeField
        objectName: style.sizeObjectName
        width: parent.width
        onEditingFinished: {
            if (style.sparse)
                style.commitSparseNumber("fontSize", text, 0, undefined)
            else
                style.commitOrSync("fontSize", Number(text))
        }
    }
    Text { text: style.alignLabel; color: "#b0b0b0"; font.pixelSize: 11 }
    ComboBox {
        id: alignCombo
        objectName: style.alignObjectName
        width: parent.width
        model: style.sparse ? ["(theme)", "left", "center", "right"]
                            : ["left", "center", "right"]
        onActivated: {
            if (style.sparse) {
                // "(theme)" is the sparse target's silence.
                if (currentIndex === 0)
                    style.resetOrSync("align")
                else
                    style.commitOrSync("align", currentValue)
            } else {
                style.commitOrSync("align", currentValue)
            }
        }
    }
    Text { text: style.spacingLabel; color: "#b0b0b0"; font.pixelSize: 11 }
    PanelField {
        id: spacingField
        objectName: style.spacingObjectName
        width: parent.width
        onEditingFinished: {
            if (style.sparse)
                style.commitSparseNumber("lineSpacing", text, 0.25, 8)
            else
                style.commitOrSync("lineSpacing", Number(text))
        }
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
        Row {
            spacing: 6
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "Weight"; color: "#b0b0b0"; font.pixelSize: 11
            }
            ComboBox {
                id: weightCombo
                objectName: style.weightObjectName
                model: [100, 200, 300, 400, 500, 600, 700, 800, 900]
                onActivated: style.commitOrSync("fontWeight", Number(currentValue))
            }
            Text {
                id: weightBadge
                anchors.verticalCenter: parent.verticalCenter
                font.pixelSize: 10
                color: "#888888"
                text: "inherited"
            }
            PanelButton {
                id: weightInherit
                objectName: style.weightInheritObjectName
                text: "Inherit"
                onClicked: style.resetOrSync("fontWeight")
            }
        }
        Row {
            spacing: 6
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "Slant"; color: "#b0b0b0"; font.pixelSize: 11
            }
            ComboBox {
                id: slantCombo
                objectName: style.slantObjectName
                model: ["normal", "italic"]
                onActivated: style.commitOrSync("fontStyle", currentValue)
            }
            PanelButton {
                id: slantInherit
                objectName: style.slantInheritObjectName
                text: "Inherit"
                onClicked: style.resetOrSync("fontStyle")
            }
        }
        Row {
            spacing: 6
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "Fit"; color: "#b0b0b0"; font.pixelSize: 11
            }
            ComboBox {
                id: fitCombo
                objectName: style.fitObjectName
                model: ["shrink", "fixed"]
                onActivated: style.commitOrSync("fitMode", currentValue)
            }
            PanelButton {
                id: fitInherit
                objectName: style.fitInheritObjectName
                text: "Inherit"
                onClicked: style.resetOrSync("fitMode")
            }
        }
        Text { text: "Minimum size (1..4096)"; color: "#b0b0b0"; font.pixelSize: 11 }
        Row {
            spacing: 4
            PanelField {
                id: minSizeField
                objectName: style.minSizeObjectName
                width: parent.width - parent.spacing - 60
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
            PanelButton {
                id: minSizeInherit
                objectName: style.minSizeInheritObjectName
                text: "Inherit"
                width: 60
                onClicked: style.resetOrSync("minFontSize")
            }
        }
    }
}
