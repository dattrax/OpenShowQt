import QtQuick
import QtQuick.Controls

// An internal Qt drag with an overlay badge, free of list clipping and scroll
// offsets. The 1px hotspot follows the pointer; the badge sits beside it.
// Source MouseAreas use drag.smoothed:false so crossing the drag threshold
// does not leave the hotspot behind the pointer by the initial movement.
Item {
    id: feedback
    parent: Overlay.overlay
    objectName: "listDragFeedback"
    property string payload: ""
    property string title: ""
    property string hint: ""
    property bool moving: false
    property bool dragging: false
    readonly property bool moveTarget: moving || (Drag.target && Drag.target.dropAction === Qt.MoveAction)
    width: 1
    height: 1
    z: 100
    visible: dragging
    Drag.active: dragging
    Drag.source: feedback
    Drag.keys: ["text/plain"]
    Drag.mimeData: ({"text/plain": feedback.payload})
    Drag.hotSpot: Qt.point(0, 0)
    Drag.supportedActions: moving ? Qt.MoveAction : Qt.CopyAction | Qt.MoveAction
    Drag.proposedAction: moving ? Qt.MoveAction : Qt.CopyAction

    function begin(source, x, y) {
        const point = source.mapToItem(feedback.parent, x, y)
        feedback.x = point.x
        feedback.y = point.y
    }

    Rectangle {
        x: 14; y: 18
        width: 242; height: 54
        radius: 5
        color: feedback.Drag.target ? "#164b78" : "#3b4753"
        border.color: feedback.Drag.target ? "#65c4ff" : "#a6bbc9"
        border.width: 2
        Text {
            x: 10; y: 7
            width: parent.width - 20
            text: (feedback.moveTarget ? "\u2195  " : "+  ") + feedback.title
            font.pixelSize: 13; font.bold: true
            color: "#ffffff"
            elide: Text.ElideRight
        }
        Text {
            x: 10; y: 29
            width: parent.width - 20
            text: feedback.Drag.target && feedback.Drag.target.dropHint
                  ? feedback.Drag.target.dropHint : feedback.hint
            font.pixelSize: 11
            color: "#d9efff"
            elide: Text.ElideRight
        }
    }
}
