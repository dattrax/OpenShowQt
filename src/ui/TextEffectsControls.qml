import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs

/*
 * UI part: TextEffectsControls; contract: docs/ui/control-sets.md#texteffectscontrols
 *
 * Outline and shadow, edited the TS.4 way (task CS.5): enable is the
 * record's own materialise — a target that carried nothing gains the
 * contract's defaults around the one member (the audience already draws
 * that look) — members then merge ONE on the controller's merge verb, and
 * a reset chip clears the whole record back to inherit. A slider is ONE
 * gesture: nothing is written while the handle is held (a drag that wrote
 * per pixel would bury the undo stack), the release commits the handle's
 * answer once, and one undo takes the whole drag back. A colour typed
 * unchanged writes nothing and costs no undo.
 *
 * The whole-record state reads visually (review 2026-11-10): a dot beside
 * the section owns this target's own record only while it speaks, the ↺
 * chip keeps its place and lights up exactly when there is a record to
 * hand back, and each colour field wears a live swatch of the value the
 * audience draws — the same visual language TextStyleControls uses for its
 * style block.
 *
 * Extracted verbatim in behaviour from ThemeEditorPage (task TS.4). A
 * target that cannot store effects at all (a text context until TV.3)
 * shows the whole section disabled, never the Songs record half-written
 * from a Bible rail.
 */
Column {
    id: effects

    required property InspectorAdapter adapter

    property string strokeCheckObjectName: "strokeEnable"
    property string strokeOffObjectName: "strokeInherit"
    property string strokeColourObjectName: "strokeColour"
    property string strokeWidthObjectName: "strokeWidth"
    property string shadowCheckObjectName: "shadowEnable"
    property string shadowOffObjectName: "shadowInherit"
    property string shadowColourObjectName: "shadowColour"
    property string shadowOpacityObjectName: "shadowOpacity"
    property string shadowOffsetXObjectName: "shadowOffsetX"
    property string shadowOffsetYObjectName: "shadowOffsetY"
    property string shadowBlurObjectName: "shadowBlur"

    spacing: 8
    width: parent ? parent.width : 0

    function state(field) { return effects.adapter.fieldState(field) }
    function sync() {
        const strokeOwn = effects.state("stroke").isSet === true
        const shadowOwn = effects.state("shadow").isSet === true
        strokeEnable.checked = effects.state("strokeEnabled").value === true
        strokeColourField.text = String(effects.state("strokeColor").value)
        strokeSwatch.color = effects.colourOf(effects.state("strokeColor").value)
        strokeWidthSlider.value = Number(effects.state("strokeWidth").value)
        strokeDot.dotColor = strokeOwn ? "#8ec9f5" : "#777777"
        strokeDot.stateSentence = effects.stateText("outline", strokeOwn)
        strokeOff.stateSentence = strokeDot.stateSentence
        // The ↺ keeps its place and dims until the target owns the record,
        // so the section's rows never jump when an edit appears.
        strokeOff.enabled = effects.state("stroke").canReset === true
        shadowEnable.checked = effects.state("shadowEnabled").value === true
        shadowColourField.text = String(effects.state("shadowColor").value)
        shadowSwatch.color = effects.colourOf(effects.state("shadowColor").value)
        shadowOpacitySlider.value = Number(effects.state("shadowOpacity").value)
        shadowOffsetXSlider.value = Number(effects.state("shadowOffsetX").value)
        shadowOffsetYSlider.value = Number(effects.state("shadowOffsetY").value)
        shadowBlurSlider.value = Number(effects.state("shadowBlurSigma").value)
        shadowDot.dotColor = shadowOwn ? "#8ec9f5" : "#777777"
        shadowDot.stateSentence = effects.stateText("shadow", shadowOwn)
        shadowOff.stateSentence = shadowDot.stateSentence
        shadowOff.enabled = effects.state("shadow").canReset === true
        // A target that cannot store effects states it and sleeps; the
        // adapter's refusal (with its reason) is the backstop.
        const storable = effects.state("strokeEnabled").editable === true
        strokeEnable.enabled = storable
        shadowEnable.enabled = storable
    }
    function stateText(record, own) {
        return own ? "this target sets its own " + record + "; click the arrow to inherit"
                   : "inherited — the chain answers"
    }
    function colourOf(value) {
        const text = String(value)
        return /^#([0-9a-fA-F]{3}|[0-9a-fA-F]{6})$/.test(text) ? text : "transparent"
    }
    // Qt's own picker (QtQuick.Dialogs), forced to the in-app dialog. The
    // swatch is the button; the hex field stays for exact values.
    function dialogColour(value) {
        const parsed = effects.colourOf(value)
        return parsed === "transparent" ? "#000000" : parsed
    }
    function hexOf(c) {
        const byte = v => ("0" + Math.max(0, Math.min(255, Math.round(v * 255))).toString(16)).slice(-2)
        return "#" + byte(c.r) + byte(c.g) + byte(c.b)
    }
    function chooseColour(field, current) {
        colourDialog.targetField = field
        colourDialog.selectedColor = effects.dialogColour(current)
        colourDialog.open()
    }
    function commitOrSync(field, value) {
        if (!effects.adapter.commit(field, value))
            effects.sync()
    }
    function resetOrSync(field) {
        if (!effects.adapter.reset(field))
            effects.sync()
    }
    // A slider drag commits once, on release, and not at all when the
    // handle ended where it began (task TS.4's rule, read through the
    // adapter so the number compared is the one the model answers with).
    function commitSlider(field, value) {
        if (Math.abs(Number(value) - Number(effects.state(field).value)) < 1e-6)
            return
        effects.commitOrSync(field, value)
    }

    Connections {
        target: effects.adapter
        function onChanged() { effects.sync() }
        function onTargetChanged() { effects.sync() }
    }
    Component.onCompleted: effects.sync()

    // The shared reset affordance (see TextStyleControls): a ↺ that keeps
    // its place and lights up only while this target owns the record it
    // would remove.
    component RevertChip: Rectangle {
        id: chip
        required property string chipField
        property string preferredName: ""
        property string stateSentence: ""
        objectName: chip.preferredName
        Accessible.name: chip.stateSentence
        width: 24
        height: 30
        radius: 4
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
            ToolTip.text: chip.stateSentence !== ""
                          ? chip.stateSentence
                          : "Hand the whole " + chip.chipField + " record back up the chain"
            onClicked: effects.resetOrSync(chip.chipField)
        }
    }

    component StateDot: Item {
        id: dot
        property string stateSentence: ""
        property color dotColor: "#777777"
        width: 9
        height: 30   // matches the checkbox/chip rows it rides beside
        Accessible.name: dot.stateSentence
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
            ToolTip.text: dot.stateSentence
        }
    }

    // --- outline ----------------------------------------------------------
    Row {
        spacing: 6
        CheckBox {
            id: strokeEnable
            objectName: effects.strokeCheckObjectName
            text: "outline"; font.pixelSize: 11
            onToggled: effects.commitOrSync("strokeEnabled", checked)
        }
        StateDot { id: strokeDot; stateSentence: effects.stateText("outline", effects.state("stroke").isSet === true) }
        RevertChip {
            id: strokeOff
            chipField: "stroke"
            preferredName: effects.strokeOffObjectName
            stateSentence: effects.stateText("outline", effects.state("stroke").isSet === true)
        }
    }
    Row {
        spacing: 4
        Text { anchors.verticalCenter: parent.verticalCenter; text: "colour"; color: "#b0b0b0"; font.pixelSize: 11 }
        Rectangle {
            id: strokeSwatch
            objectName: effects.strokeColourObjectName + "Swatch"
            anchors.verticalCenter: parent.verticalCenter
            width: 20; height: 20; radius: 3
            color: "transparent"
            border.color: "#6a6a6d"
            Accessible.name: "Choose outline colour"
            MouseArea {
                anchors.fill: parent
                enabled: strokeColourField.enabled
                cursorShape: Qt.PointingHandCursor
                onClicked: effects.chooseColour("strokeColor", strokeColourField.text)
            }
        }
        PanelField {
            id: strokeColourField
            objectName: effects.strokeColourObjectName
            anchors.verticalCenter: parent.verticalCenter
            width: Math.max(70, effects.width - 96)
            onEditingFinished: {
                if (text.trim() === String(effects.state("strokeColor").value))
                    return  // focus-out with no change writes nothing
                effects.commitOrSync("strokeColor", text.trim())
            }
        }
    }
    Row {
        spacing: 4
        Text { text: "width"; color: "#b0b0b0"; font.pixelSize: 11 }
        Slider {
            id: strokeWidthSlider
            objectName: effects.strokeWidthObjectName
            from: 0; to: 32; stepSize: 0.5
            onPressedChanged: if (!pressed)
                effects.commitSlider("strokeWidth", value)
        }
    }

    // --- shadow -----------------------------------------------------------
    Row {
        spacing: 6
        CheckBox {
            id: shadowEnable
            objectName: effects.shadowCheckObjectName
            text: "shadow"; font.pixelSize: 11
            onToggled: effects.commitOrSync("shadowEnabled", checked)
        }
        StateDot { id: shadowDot; stateSentence: effects.stateText("shadow", effects.state("shadow").isSet === true) }
        RevertChip {
            id: shadowOff
            chipField: "shadow"
            preferredName: effects.shadowOffObjectName
            stateSentence: effects.stateText("shadow", effects.state("shadow").isSet === true)
        }
    }
    Row {
        spacing: 4
        Text { anchors.verticalCenter: parent.verticalCenter; text: "colour"; color: "#b0b0b0"; font.pixelSize: 11 }
        Rectangle {
            id: shadowSwatch
            objectName: effects.shadowColourObjectName + "Swatch"
            anchors.verticalCenter: parent.verticalCenter
            width: 20; height: 20; radius: 3
            color: "transparent"
            border.color: "#6a6a6d"
            Accessible.name: "Choose shadow colour"
            MouseArea {
                anchors.fill: parent
                enabled: shadowColourField.enabled
                cursorShape: Qt.PointingHandCursor
                onClicked: effects.chooseColour("shadowColor", shadowColourField.text)
            }
        }
        PanelField {
            id: shadowColourField
            objectName: effects.shadowColourObjectName
            anchors.verticalCenter: parent.verticalCenter
            width: Math.max(70, effects.width - 96)
            onEditingFinished: {
                if (text.trim() === String(effects.state("shadowColor").value))
                    return
                effects.commitOrSync("shadowColor", text.trim())
            }
        }
    }
    Row {
        spacing: 4
        Text { text: "opacity"; color: "#b0b0b0"; font.pixelSize: 11 }
        Slider {
            id: shadowOpacitySlider
            objectName: effects.shadowOpacityObjectName
            from: 0; to: 1; stepSize: 0.05
            onPressedChanged: if (!pressed)
                effects.commitSlider("shadowOpacity", value)
        }
    }
    Row {
        spacing: 4
        Text { text: "offset X"; color: "#b0b0b0"; font.pixelSize: 11 }
        Slider {
            id: shadowOffsetXSlider
            objectName: effects.shadowOffsetXObjectName
            from: -256; to: 256; stepSize: 1
            onPressedChanged: if (!pressed)
                effects.commitSlider("shadowOffsetX", value)
        }
    }
    Row {
        spacing: 4
        Text { text: "offset Y"; color: "#b0b0b0"; font.pixelSize: 11 }
        Slider {
            id: shadowOffsetYSlider
            objectName: effects.shadowOffsetYObjectName
            from: -256; to: 256; stepSize: 1
            onPressedChanged: if (!pressed)
                effects.commitSlider("shadowOffsetY", value)
        }
    }
    Row {
        spacing: 4
        Text { text: "blur σ"; color: "#b0b0b0"; font.pixelSize: 11 }
        Slider {
            id: shadowBlurSlider
            objectName: effects.shadowBlurObjectName
            from: 0; to: 64; stepSize: 0.5
            onPressedChanged: if (!pressed)
                effects.commitSlider("shadowBlurSigma", value)
        }
    }

    ColorDialog {
        id: colourDialog
        objectName: effects.strokeColourObjectName + "Dialog"
        title: "Choose a colour"
        options: ColorDialog.DontUseNativeDialog
        property string targetField: ""
        onAccepted: {
            if (colourDialog.targetField !== "")
                effects.commitOrSync(colourDialog.targetField,
                                     effects.hexOf(colourDialog.selectedColor))
            colourDialog.targetField = ""
        }
    }
}
