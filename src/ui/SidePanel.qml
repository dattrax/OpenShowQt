import QtQuick

/*
 * A resizable side panel of the workspace shell (task 4.1).
 *
 * Faithful to the reference (docs/reference.md §2): the panel itself is
 * the drag target — a press within the handle strip on its inner edge
 * resizes it — a collapsed panel shows a white arrow in the strip and a
 * click on that strip restores it, while an open panel ignores clicks
 * (the reference never closes a panel by accident). The snap rules are
 * WorkspacePanels' C++ (fixture-tested); this file only turns mouse
 * positions into candidate widths: for the left dock that is the mouse's
 * window-x (the reference uses clientX directly), for the right dock the
 * width minus the distance travelled.
 */
Rectangle {
    id: dock

    required property string side  // "left" | "right"
    signal draggedTo(real candidate)
    signal stripClicked()

    // The reference's handle: 4px normally, 6 once the panel is closed to
    // 8 or under, which is the same strip that carries the restore arrow.
    readonly property bool collapsed: width <= 8
    readonly property real handleWidth: collapsed ? 6 : 4

    default property alias content: contentArea.data

    color: "#353535"  // --primary
    clip: true

    Item {
        id: contentArea
        visible: !dock.collapsed
        anchors.fill: parent
        anchors.leftMargin: dock.side === "right" ? dock.handleWidth : 0
        anchors.rightMargin: dock.side === "left" ? dock.handleWidth : 0
    }

    MouseArea {
        id: handle
        width: dock.handleWidth
        height: parent.height
        x: dock.side === "left" ? parent.width - width : 0
        cursorShape: Qt.SplitHCursor
        hoverEnabled: true

        property real pressX: 0
        property real pressWidth: 0
        property bool travelled: false

        onPressed: function (mouse) {
            pressX = mapToItem(null, mouse.x, 0).x
            pressWidth = dock.width
            travelled = false
        }
        onPositionChanged: function (mouse) {
            if (!pressed) return
            if (Math.abs(mapToItem(null, mouse.x, 0).x - pressX) < 3 && !travelled) return
            travelled = true
            const x = mapToItem(null, mouse.x, 0).x
            // Left: width is the absolute scene-x of the pointer (the panel
            // spans from the window edge). Right: the reference fixes the
            // offset at press and measures back from the width it started
            // at - using the live width here would compound each event.
            dock.draggedTo(dock.side === "left" ? x : pressWidth - (x - pressX))
        }
        onClicked: {
            // Only a true click reaches the strip action, and only a
            // closed panel responds (the reference's click-to-open).
            if (!travelled) {
                dock.stripClicked()
            }
        }
    }

    // The strip's quiet highlight, so the drag affordance reads.
    Rectangle {
        width: dock.handleWidth
        height: parent.height
        x: dock.side === "left" ? parent.width - width : 0
        visible: handle.containsMouse
        color: "#4a4a4a"  // --primary-lighter
    }

    // Collapsed: the white arrow in the strip (the reference's arrow_right),
    // pointing at the direction the panel will travel when restored.
    Canvas {
        width: 6
        height: 14
        anchors.verticalCenter: parent.verticalCenter
        x: dock.side === "left" ? 0 : parent.width - width
        visible: dock.collapsed
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            ctx.fillStyle = "#ffffff"
            ctx.beginPath()
            if (dock.side === "left") {
                ctx.moveTo(1, 2)
                ctx.lineTo(5, 7)
                ctx.lineTo(1, 12)
            } else {
                ctx.moveTo(5, 2)
                ctx.lineTo(1, 7)
                ctx.lineTo(5, 12)
            }
            ctx.closePath()
            ctx.fill()
        }
        onVisibleChanged: requestPaint()
    }
}
