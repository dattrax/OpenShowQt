import QtQuick
import QtQuick.Controls

/*
 * UI part: BackgroundControls; contract: docs/ui/control-sets.md#backgroundcontrols
 *
 * The colour rows an appearance target stores (task CS.5): text colour,
 * background colour, and the gradient colour whose empty string is the
 * EXPLICIT flat — distinct from inherit, which these base fields never
 * have (a theme is the root of its own colours; the adapter refuses the
 * reset and says so). Extracted verbatim in behaviour from
 * ThemeEditorPage's Songs rail; image/fit rows join with the slide target
 * (CS.6) through the same part.
 *
 * Every commit restates the WHOLE base record through the adapter's
 * editTheme route — one omitted field cannot silently flatten a gradient —
 * and a refusal puts the field back where the model says it belongs.
 *
 * Background images and their fit are NOT part of this row set: they are
 * properties of the show's slide (docs.assets, setSlideBackground), not
 * fields an AppearanceTarget answers for, so the Edit page keeps its own
 * asset combo beside this section. What every target with colours can
 * show is the revert chip (CS.6): `showRevertChips` puts a small ↺ beside
 * each colour the target OWNS (canReset), and one click hands that colour
 * back up the chain. Themes never own their base colours, so a Songs rail
 * shows no chip; a customized slide shows one per customized colour.
 */
Column {
    id: background

    required property InspectorAdapter adapter

    // The Edit page's badge tradition: objectName is the prefix plus the
    // field with its first letter raised (customizeBadgeTextColor).
    property bool showRevertChips: false
    property string revertBadgePrefix: "customizeBadge"

    property string textColorObjectName: "textColourField"
    property string backColorObjectName: "backgroundColorField"
    property string gradientObjectName: "gradientField"

    spacing: 8
    width: parent ? parent.width : 0

    function state(field) { return background.adapter.fieldState(field) }
    function capitalise(field) {
        return field.charAt(0).toUpperCase() + field.substring(1)
    }
    function sync() {
        const text = background.state("textColor")
        const back = background.state("backgroundColor")
        const gradient = background.state("backgroundGradient")
        textField.text = text.value === undefined ? "" : String(text.value)
        backField.text = back.value === undefined ? "" : String(back.value)
        gradientField.text = gradient.value === undefined ? "" : String(gradient.value)
        const storable = text.editable === true
        textField.enabled = storable
        backField.enabled = storable
        gradientField.enabled = storable
        // Which chips stand up is an answer from the model, so it is
        // assigned here, not bound: the adapter's reads are function calls
        // a binding cannot follow.
        textChip.visible = background.showRevertChips && text.canReset === true
        backChip.visible = background.showRevertChips && back.canReset === true
        gradientChip.visible = background.showRevertChips
                               && gradient.canReset === true
    }
    function commitOrSync(field, value) {
        if (!background.adapter.commit(field, value))
            background.sync()
    }
    // A focus-out with nothing changed is not an edit — editingFinished
    // fires on a bare focus loss, and a field merely looked at customizes
    // nothing and costs no undo (the rule EditPage's panel always had, and
    // the capture's "exactly two customized" depends on). A deliberate
    // combo choice or checkbox click still commits as stated: a selection
    // that matches what the audience draws today MATERIALIZES, and that
    // belongs to the explicit rows, never to a focus-out.
    function commitIfChanged(field, typed) {
        if (typed === String(background.state(field).value)) {
            background.sync()
            return
        }
        background.commitOrSync(field, typed)
    }
    function resetOrSync(field) {
        if (!background.adapter.reset(field))
            background.sync()
    }

    // The Edit page's RevertBadge, extracted: a 24x30 ↺ that only stands
    // up while the field has something under it to put back.
    component RevertChip: Rectangle {
        id: chip
        required property string chipField
        objectName: background.revertBadgePrefix + background.capitalise(chip.chipField)
        Accessible.name: "Revert " + chip.chipField + " customization"
        width: 24
        height: 30 // the PanelField's height, so a Row lines them up
        radius: 4
        visible: false // sync() decides, from the model's canReset
        color: chipArea.containsMouse ? "#565659" : "#3a3a3c"
        border.color: "#6a6a6d"
        Text {
            anchors.centerIn: parent
            color: "#ffd666"
            font.pixelSize: 13
            text: "\u21ba"
        }
        MouseArea {
            id: chipArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: background.resetOrSync(chip.chipField)
        }
    }

    Connections {
        target: background.adapter
        function onChanged() { background.sync() }
        function onTargetChanged() { background.sync() }
    }
    Component.onCompleted: background.sync()

    Text { text: "Text colour (#rgb or #rrggbb)"; color: "#b0b0b0"; font.pixelSize: 11 }
    Row {
        spacing: 2
        PanelField {
            id: textField
            objectName: background.textColorObjectName
            width: background.width - (textChip.visible ? 26 : 0)
            onEditingFinished: background.commitIfChanged("textColor", text.trim())
        }
        RevertChip { id: textChip; chipField: "textColor" }
    }
    readonly property bool supportsBackdrop: background.adapter.target.kind === "slide"

    Text { visible: background.supportsBackdrop; text: "Background colour"; color: "#b0b0b0"; font.pixelSize: 11 }
    Row {
        visible: background.supportsBackdrop
        spacing: 2
        PanelField {
            id: backField
            objectName: background.backColorObjectName
            width: background.width - (backChip.visible ? 26 : 0)
            onEditingFinished: background.commitIfChanged("backgroundColor", text.trim())
        }
        RevertChip { id: backChip; chipField: "backgroundColor" }
    }
    Text { visible: background.supportsBackdrop; text: "Gradient colour (\"\" = flat)"; color: "#b0b0b0"; font.pixelSize: 11 }
    Row {
        visible: background.supportsBackdrop
        spacing: 2
        PanelField {
            id: gradientField
            objectName: background.gradientObjectName
            width: background.width - (gradientChip.visible ? 26 : 0)
            // "" stays the EXPLICIT flat: the base record always stores a
            // gradient member, so this writes "" rather than inheriting.
            onEditingFinished: background.commitIfChanged("backgroundGradient", text.trim())
        }
        RevertChip { id: gradientChip; chipField: "backgroundGradient" }
    }
}
