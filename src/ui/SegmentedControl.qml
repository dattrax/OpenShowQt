import QtQuick
import QtQuick.Controls

/*
 * UI part: SegmentedControl; contract: docs/ui/control-sets.md#segmentedcontrol
 *
 * A small enumeration shown as a row of joined buttons, so the ACTIVE
 * choice is visible at a glance instead of hidden behind a closed combo.
 * The API is the subset of ComboBox the shared style rows already drive —
 * `model`, `currentIndex`, `count`, `currentValue` and `activated(index)` —
 * so a row can swap a picker for this part without changing how its host
 * reads or commits it. A programmatic `currentIndex` assignment (a sync
 * from the record) never emits `activated`, exactly as a ComboBox does
 * not; only a click does.
 *
 * `model` entries may be strings or objects with `{value, label}`.
 * `glyphStyle: "align"` draws the three paragraph-alignment marks instead
 * of words; `italicValues` slants an option whose value is "italic".
 * The former sparse choice "(theme)" is shown as "Theme" rather than a
 * parenthesis soup — it still commits as the value the host checks for.
 */
Item {
    id: control

    property var model: []
    property int currentIndex: -1
    readonly property int count: control.model ? control.model.length : 0
    // "" | "align" | "vertical"
    property string glyphStyle: ""
    property bool italicValues: false

    signal activated(int index)

    implicitWidth: 150
    implicitHeight: 30

    function optionAt(i) {
        if (!control.model || i < 0 || i >= control.model.length)
            return undefined
        return control.model[i]
    }
    function valueAt(i) {
        const option = control.optionAt(i)
        if (option === undefined || option === null)
            return ""
        if (typeof option === "object")
            return String(option.value !== undefined ? option.value
                                                     : (option.label !== undefined ? option.label : ""))
        return String(option)
    }
    function activate(i) {
        if (i < 0 || i >= control.count)
            return
        control.currentIndex = i
        control.activated(i)
    }
    function labelAt(i) {
        const option = control.optionAt(i)
        if (option === undefined || option === null)
            return ""
        if (typeof option === "object")
            return String(option.label !== undefined ? option.label : control.valueAt(i))
        return option === "(theme)" ? "Theme" : String(option)
    }
    readonly property string currentValue: control.valueAt(control.currentIndex)

    Rectangle {
        anchors.fill: parent
        radius: 4
        color: "#2b2b2c"
        border.color: "#4a4a4a"
        border.width: 1

        Row {
            anchors.fill: parent
            anchors.margins: 1
            spacing: 0

            Repeater {
                model: control.count
                delegate: Rectangle {
                    id: segment
                    required property int index
                    // Task TL.5: each segment names itself so an accessible
                    // name, a tooltip and a keyboard fixture can address the
                    // exact choice without reaching into the Row.
                    objectName: control.objectName !== ""
                               ? control.objectName + "Segment" + segment.index : ""
                    width: control.count > 0 ? (control.width - 2) / control.count : 0
                    height: parent.height
                    radius: 3
                    color: segment.index === control.currentIndex
                           ? "#1685f8"
                           : (segmentArea.containsMouse ? "#3d3d40" : "transparent")

                    Accessible.name: control.labelAt(segment.index)
                    Accessible.role: Accessible.RadioButton
                    Accessible.checked: segment.index === control.currentIndex
                    Accessible.focusable: true
                    // Task TL.5: every segment is reachable by Tab and
                    // activated by Space/Return as well as by a click. The
                    // focus ring is drawn on the segment itself so the
                    // shared rows need no extra part.
                    activeFocusOnTab: true
                    focus: false
                    border.width: segment.activeFocus ? 1 : 0
                    border.color: "#ffd666"
                    Keys.onSpacePressed: control.activate(segment.index)
                    Keys.onReturnPressed: control.activate(segment.index)
                    Keys.onEnterPressed: control.activate(segment.index)

                    // Alignment marks: three lines whose ragged edge names
                    // the choice. "(theme)" keeps its word; the mark would
                    // have to mean "whatever the chain says", which is a
                    // sentence, not a glyph.
                    Item {
                        anchors.centerIn: parent
                        width: 16
                        height: 11
                        visible: control.glyphStyle === "align"
                                 && control.valueAt(segment.index) !== "(theme)"
                        Repeater {
                            model: [16, 10, 16]
                            delegate: Rectangle {
                                required property int index
                                required property var modelData
                                width: modelData
                                height: 1.5
                                y: index * 4.5
                                color: segment.index === control.currentIndex
                                       ? "#ffffff" : "#cccccc"
                                x: {
                                    const align = control.valueAt(segment.index)
                                    if (align === "center")
                                        return (16 - width) / 2
                                    if (align === "right")
                                        return 16 - width
                                    return 0
                                }
                            }
                        }
                    }

                    // Vertical marks: three text lines offset as a block
                    // against a full-height guide, so top/centre/bottom is
                    // legible without a platform icon font or PowerPoint
                    // artwork. The guide is the paragraph's box; the lines
                    // sit at its top, middle or bottom.
                    Item {
                        anchors.centerIn: parent
                        width: 16
                        height: 13
                        visible: control.glyphStyle === "vertical"
                        Rectangle {
                            width: 1
                            height: parent.height
                            x: 0
                            color: "#6a6a6d"
                        }
                        Repeater {
                            model: 3
                            delegate: Rectangle {
                                required property int index
                                width: 11
                                height: 1.5
                                x: 3
                                color: segment.index === control.currentIndex
                                       ? "#ffffff" : "#cccccc"
                                y: {
                                    const anchor = control.valueAt(segment.index)
                                    const block = 8.0   // three lines, 2.5 apart
                                    const free = 13.0 - block
                                    if (anchor === "top")
                                        return index * 4.0
                                    if (anchor === "bottom")
                                        return free + index * 4.0
                                    return free / 2.0 + index * 4.0
                                }
                            }
                        }
                    }

                    Text {
                        anchors.centerIn: parent
                        visible: !(control.glyphStyle === "align"
                                   && control.valueAt(segment.index) !== "(theme)")
                                 && control.glyphStyle !== "vertical"
                        text: control.labelAt(segment.index)
                        color: segment.index === control.currentIndex
                               ? "#ffffff" : "#cccccc"
                        font.pixelSize: 11
                        font.italic: control.italicValues
                                     && control.valueAt(segment.index) === "italic"
                    }

                    MouseArea {
                        id: segmentArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        ToolTip.visible: containsMouse && control.glyphStyle !== ""
                        ToolTip.text: control.labelAt(segment.index)
                        onClicked: {
                            segment.forceActiveFocus()
                            control.activate(segment.index)
                        }
                    }
                }
            }
        }
    }
}
