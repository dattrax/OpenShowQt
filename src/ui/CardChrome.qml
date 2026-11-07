import QtQuick

/*
 * UI part: CardChrome; contract: docs/ui/control-sets.md#cardchrome
 *
 * The shared card surface: selection colour, hover colour, border and radius.
 * Display-only chrome — the host overlays its own MouseArea so drag, click and
 * context semantics stay with the wearer. MediaCard and the Library's row
 * cards wear it, so the two cannot drift on selection or hover colour.
 */
Rectangle {
    id: chrome

    property bool selected: false
    property bool hovered: false
    property color idleColor: "#242424"
    property color hoverColor: "#3a3a3a"
    property color selectedColor: "#1d3b5c"
    property color selectedBorder: "#1685f8"
    property color idleBorder: "#4a4a4a"
    property real borderWidth: 1
    property real selectedBorderWidth: 2
    property real radius: 2

    default property alias content: contentItem.data

    color: selected ? selectedColor : (hovered ? hoverColor : idleColor)
    border.width: selected ? selectedBorderWidth : borderWidth
    border.color: selected ? selectedBorder : idleBorder

    Item {
        id: contentItem
        anchors.fill: parent
    }
}
