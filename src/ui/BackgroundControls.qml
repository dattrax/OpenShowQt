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
 */
Column {
    id: background

    required property InspectorAdapter adapter
    // Image/fit rows belong to the slide target (CS.6); nothing else can
    // store them today, so a host that cannot pick says so and no button
    // appears.
    property bool canPickImage: false

    property string textColorObjectName: "textColourField"
    property string backColorObjectName: "backgroundColorField"
    property string gradientObjectName: "gradientField"

    signal backgroundChosen(string assetId)
    signal backgroundFitChosen(string name)

    spacing: 8
    width: parent ? parent.width : 0

    function state(field) { return background.adapter.fieldState(field) }
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
    }
    function commitOrSync(field, value) {
        if (!background.adapter.commit(field, value))
            background.sync()
    }

    Connections {
        target: background.adapter
        function onChanged() { background.sync() }
        function onTargetChanged() { background.sync() }
    }
    Component.onCompleted: background.sync()

    Text { text: "Text colour (#rgb or #rrggbb)"; color: "#b0b0b0"; font.pixelSize: 11 }
    PanelField {
        id: textField
        objectName: background.textColorObjectName
        width: parent.width
        onEditingFinished: background.commitOrSync("textColor", text)
    }
    Text { text: "Background colour"; color: "#b0b0b0"; font.pixelSize: 11 }
    PanelField {
        id: backField
        objectName: background.backColorObjectName
        width: parent.width
        onEditingFinished: background.commitOrSync("backgroundColor", text)
    }
    Text { text: "Gradient colour (\"\" = flat)"; color: "#b0b0b0"; font.pixelSize: 11 }
    PanelField {
        id: gradientField
        objectName: background.gradientObjectName
        width: parent.width
        // "" stays the EXPLICIT flat: the base record always stores a
        // gradient member, so this writes "" rather than inheriting.
        onEditingFinished: background.commitOrSync("backgroundGradient", text)
    }
}
