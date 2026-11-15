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
    // "" | "align"
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
                    width: control.count > 0 ? (control.width - 2) / control.count : 0
                    height: parent.height
                    radius: 3
                    color: segment.index === control.currentIndex
                           ? "#1685f8"
                           : (segmentArea.containsMouse ? "#3d3d40" : "transparent")

                    Accessible.name: control.labelAt(segment.index)
                    Accessible.role: Accessible.RadioButton
                    Accessible.checked: segment.index === control.currentIndex

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

                    Text {
                        anchors.centerIn: parent
                        visible: !(control.glyphStyle === "align"
                                   && control.valueAt(segment.index) !== "(theme)")
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
                        onClicked: {
                            control.currentIndex = segment.index
                            control.activated(segment.index)
                        }
                    }
                }
            }
        }
    }
}
