import QtQuick
import QtQuick.Controls

/*
 * UI part: TextEffectsControls; contract: docs/ui/control-sets.md#texteffectscontrols
 *
 * Outline and shadow, edited the TS.4 way (task CS.5): enable is the
 * record's own materialise — a target that carried nothing gains the
 * contract's defaults around the one member (the audience already draws
 * that look) — members then merge ONE on the controller's merge verb, and
 * "Off" clears the whole record back to inherit. A slider is ONE gesture:
 * nothing is written while the handle is held (a drag that wrote per pixel
 * would bury the undo stack), the release commits the handle's answer once,
 * and one undo takes the whole drag back. A colour typed unchanged writes
 * nothing and costs no undo.
 *
 * Extracted verbatim in behaviour from ThemeEditorPage (task TS.4). Badges
 * say whether this target owns the record or previews what the audience
 * draws today; a target that cannot store effects at all (a text context
 * until TV.3) shows the whole section disabled, never the Songs record
 * half-written from a Bible rail.
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
        strokeWidthSlider.value = Number(effects.state("strokeWidth").value)
        strokeBadge.text = strokeOwn ? "own" : "inherited"
        strokeBadge.color = strokeOwn ? "#8ec9f5" : "#888888"
        strokeOff.enabled = effects.state("stroke").canReset === true
        shadowEnable.checked = effects.state("shadowEnabled").value === true
        shadowColourField.text = String(effects.state("shadowColor").value)
        shadowOpacitySlider.value = Number(effects.state("shadowOpacity").value)
        shadowOffsetXSlider.value = Number(effects.state("shadowOffsetX").value)
        shadowOffsetYSlider.value = Number(effects.state("shadowOffsetY").value)
        shadowBlurSlider.value = Number(effects.state("shadowBlurSigma").value)
        shadowBadge.text = shadowOwn ? "own" : "inherited"
        shadowBadge.color = shadowOwn ? "#8ec9f5" : "#888888"
        shadowOff.enabled = effects.state("shadow").canReset === true
        // A target that cannot store effects states it and sleeps; the
        // adapter's refusal (with its reason) is the backstop.
        const storable = effects.state("strokeEnabled").editable === true
        strokeEnable.enabled = storable
        shadowEnable.enabled = storable
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

    // --- outline ----------------------------------------------------------
    Row {
        spacing: 6
        CheckBox {
            id: strokeEnable
            objectName: effects.strokeCheckObjectName
            text: "outline"; font.pixelSize: 11
            onToggled: effects.commitOrSync("strokeEnabled", checked)
        }
        PanelButton {
            id: strokeOff
            objectName: effects.strokeOffObjectName
            text: "Off"
            onClicked: effects.resetOrSync("stroke")
        }
        Text {
            id: strokeBadge
            anchors.verticalCenter: parent.verticalCenter
            font.pixelSize: 10
            color: "#888888"
            text: "inherited"
        }
    }
    Row {
        spacing: 4
        Text { text: "colour"; color: "#b0b0b0"; font.pixelSize: 11 }
        PanelField {
            id: strokeColourField
            objectName: effects.strokeColourObjectName
            width: 110
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
        PanelButton {
            id: shadowOff
            objectName: effects.shadowOffObjectName
            text: "Off"
            onClicked: effects.resetOrSync("shadow")
        }
        Text {
            id: shadowBadge
            anchors.verticalCenter: parent.verticalCenter
            font.pixelSize: 10
            color: "#888888"
            text: "inherited"
        }
    }
    Row {
        spacing: 4
        Text { text: "colour"; color: "#b0b0b0"; font.pixelSize: 11 }
        PanelField {
            id: shadowColourField
            objectName: effects.shadowColourObjectName
            width: 110
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
}
