import QtQuick
import QtQuick.Controls

/*
 * UI part: BoxGeometryControls; contract: docs/ui/control-sets.md#boxgeometrycontrols
 *
 * The X/Y/W/H rows every box target shares (task CS.5): four fields that
 * commit as ONE unit because a placement is one unit and a half-typed one
 * is a wrong one. The adapter routes "box" to whatever the host calls its
 * rectangle — setThemePlacement/clearThemePlacement for themes, the merged
 * context box (retiring a stored inset in the same edit) for views, the
 * single placement customization for slides — so a drag and the typed
 * path commit the same one undo either way.
 *
 * Extracted verbatim in behaviour from ThemeEditorPage (themes-plan
 * amendment 2026-11-01 and task TV.1): silence SHOWS the rectangle the
 * words would land on anyway (the resolved box), because that is the
 * honest answer to "where do the words go right now"; typing over it
 * states this target's own opinion. Validation here is shape only —
 * four finite numbers with w/h >= 1 (this replaces the context's earlier
 * w>0 rule: a box thinner than a pixel cannot be placed either way, and
 * the domain still has the last word) — and any refusal puts every field
 * back where the record says it belongs.
 */
Column {
    id: geometry

    required property InspectorAdapter adapter
    property string label: "Lyric box (x, y, w, h in 1920×1080)"
    // The removal button ("No placement" / "No box") and, for the Bible
    // view, the extra action that borrows the carrying theme's rectangle.
    property string clearLabel: "No placement"
    property string extraActionLabel: ""
    // The Songs rail states whether this target places at all; the Bible
    // rail says nothing beside the buttons.
    property bool showStatus: true
    property string statusOwnedText: "this theme places its slides"
    property string statusSilentText: "no opinion — slides keep their own boxes"

    property string xObjectName: "boxXField"
    property string yObjectName: "boxYField"
    property string wObjectName: "boxWField"
    property string hObjectName: "boxHField"
    property string clearObjectName: "boxClear"
    property string extraObjectName: "boxExtra"
    property string statusObjectName: "boxStatus"

    // The host owns the gesture and any extra verb; the part draws the
    // typed path through the adapter and forwards the extra button.
    signal extraClicked()

    spacing: 8
    width: parent ? parent.width : 0

    function state() { return geometry.adapter.fieldState("box") }
    function sync() {
        const s = geometry.state()
        const box = s.value
        const has = box !== undefined && box !== null && box.x !== undefined
        xField.text = has ? String(Math.round(box.x)) : ""
        yField.text = has ? String(Math.round(box.y)) : ""
        wField.text = has ? String(Math.round(box.width)) : ""
        hField.text = has ? String(Math.round(box.height)) : ""
        clearButton.enabled = s.canReset === true && s.editable === true
        extraButton.enabled = s.editable === true
        xField.enabled = s.editable === true
        yField.enabled = s.editable === true
        wField.enabled = s.editable === true
        hField.enabled = s.editable === true
        statusText.text = s.isSet === true ? geometry.statusOwnedText
                                           : geometry.statusSilentText
        statusText.color = s.isSet === true ? "#8ec9f5" : "#888888"
    }

    Connections {
        target: geometry.adapter
        function onChanged() { geometry.sync() }
        function onTargetChanged() { geometry.sync() }
    }
    Component.onCompleted: geometry.sync()

    Text {
        text: geometry.label
        color: "#b0b0b0"
        font.pixelSize: 11
    }
    Row {
        spacing: 4
        PanelField {
            id: xField
            objectName: geometry.xObjectName
            width: 52
            onEditingFinished: geometry.commitFields()
        }
        PanelField {
            id: yField
            objectName: geometry.yObjectName
            width: 52
            onEditingFinished: geometry.commitFields()
        }
        PanelField {
            id: wField
            objectName: geometry.wObjectName
            width: 52
            onEditingFinished: geometry.commitFields()
        }
        PanelField {
            id: hField
            objectName: geometry.hObjectName
            width: 52
            onEditingFinished: geometry.commitFields()
        }
    }
    Row {
        spacing: 6
        PanelButton {
            id: extraButton
            objectName: geometry.extraObjectName
            visible: geometry.extraActionLabel !== ""
            text: geometry.extraActionLabel
            onClicked: geometry.extraClicked()
        }
        PanelButton {
            id: clearButton
            objectName: geometry.clearObjectName
            text: geometry.clearLabel
            onClicked: {
                if (!geometry.adapter.reset("box"))
                    geometry.sync()
            }
        }
        Text {
            id: statusText
            visible: geometry.showStatus
            anchors.verticalCenter: parent.verticalCenter
            color: "#888888"
            font.pixelSize: 11
        }
    }

    // The exact-number path: four fields commit together as one unit;
    // anything the page will not place (or the model will not hold) puts
    // the fields back where the record says they belong.
    function commitFields() {
        const x = parseFloat(xField.text)
        const y = parseFloat(yField.text)
        const w = parseFloat(wField.text)
        const h = parseFloat(hField.text)
        if (!Number.isFinite(x) || !Number.isFinite(y)
                || !(w >= 1) || !(h >= 1)
                || !geometry.adapter.commit("box", {x: x, y: y, width: w, height: h}))
            geometry.sync()
    }
}
