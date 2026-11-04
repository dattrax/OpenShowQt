import QtQuick

/*
 * UI part: SlidePreviewCard; contract: docs/ui/control-sets.md#slidepreviewcard
 *
 * A SlidePreviewSurface wearing the chrome of a selectable list item (task
 * CS.2): a body that answers to hover, a border that reports the worker's
 * acknowledged LIVE and the operator's own pick without ever implying the
 * other, and a caption strip that names the row. The Show cue tiles are the
 * first wearers; the Bible generated-slide cards join in CS.3.
 *
 * A card is a CLICK TARGET, not a focusable button: on the Show screen a
 * focused view would eat the presenter's Space and PageUp/PageDown after any
 * click, and stepping cues with the keyboard must survive browsing. What an
 * activation MEANS stays with the host — the card never calls a presentation
 * verb — and state chrome is computed by the host from acknowledged worker
 * facts, never from the click that asked.
 */
Item {
    id: card

    // State chrome. `live` is the worker's fact and outranks everything;
    // `selected` is the operator's browse cursor.
    property bool selected: false
    property bool live: false
    property string caption: ""

    property color idleColor: "#2d2d2d"
    property color hoverColor: "#393939"

    // The picture and everything the host pins over it (acknowledgement
    // chips, badges) go in the surface / overlay slot respectively.
    readonly property alias surface: previewSurface
    default property alias overlay: previewSurface.overlay

    // Primary click, and a right-click in CARD coordinates — the host maps
    // it wherever its menu lives.
    signal activated()
    signal contextRequested(real mouseX, real mouseY)

    Rectangle {
        anchors.fill: parent
        color: tileMouse.containsMouse ? card.hoverColor : card.idleColor
        // Live is the worker's fact (red, thickest); the operator's own pick
        // is blue; anything else is a quiet outline.
        border.width: card.live ? 3 : (card.selected ? 2 : 1)
        border.color: card.live ? "#d24b4b"
                     : (card.selected ? "#1685f8" : "#505050")
    }

    SlidePreviewSurface {
        id: previewSurface
        x: 3
        y: 3
        width: card.width - 6
        height: width * aspectRatio
        borderColor: "transparent"
        borderWidth: 0
    }

    // The caption strip fills what the picture leaves — the same room the
    // cue tiles have always reserved under the thumbnail.
    Rectangle {
        visible: card.caption !== ""
        x: 2
        y: previewSurface.y + previewSurface.height + 2
        width: card.width - 4
        height: Math.max(0, card.height - y - 2)
        color: card.live ? "#7c2c28" : (card.selected ? "#1678cf" : "#414141")
        Text {
            anchors.fill: parent
            anchors.leftMargin: 6
            anchors.rightMargin: 6
            verticalAlignment: Text.AlignVCenter
            text: card.caption
            color: "#eeeeee"
            font.pixelSize: 12
            elide: Text.ElideRight
        }
    }

    MouseArea {
        id: tileMouse
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onClicked: function (mouse) {
            if (mouse.button === Qt.RightButton) {
                card.contextRequested(mouse.x, mouse.y)
            } else {
                card.activated()
            }
        }
    }
}
