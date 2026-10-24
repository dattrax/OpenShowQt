import QtQuick

/*
 * The workspace top bar (task 4.1, docs/reference.md §2): 40px tall,
 * title left, page buttons centred, output state right — plus the
 * reference's unsaved marker, a 5x40 red strip at the left edge while
 * the document has unwritten changes. Only the show page has content
 * yet: edit arrives with the editor tasks (4.3/4.4), stage/draw follow
 * the reference's later pages, and settings with 4.5, so those buttons
 * sit present but disabled rather than pretending.
 */
Rectangle {
    id: bar

    height: 40
    color: "#242832"  // --primary

    Rectangle {
        width: 5
        height: 40
        color: "#40ff0000"  // rgba(255, 0, 0, .25)
        visible: docs.dirty
    }

    Text {
        x: 16
        anchors.verticalCenter: parent.verticalCenter
        color: "#f0f0ff"  // --text
        font.pixelSize: 14
        font.bold: true
        text: "OpenShowQt"
    }

    // Page buttons, centred like the reference's Top.svelte.
    Row {
        anchors.centerIn: parent
        spacing: 4

        Repeater {
            model: ["show", "edit", "stage", "draw", "settings"]

            delegate: Item {
                id: pageButton

                required property string modelData
                readonly property bool active: modelData === "show"
                width: label.width + 20
                height: 40

                Text {
                    id: label
                    anchors.centerIn: parent
                    color: pageButton.active ? "#f0f0ff" : "#565b6b"
                    font.pixelSize: 13
                    text: pageButton.modelData
                }

                Rectangle {
                    width: parent.width
                    height: 2
                    anchors.bottom: parent.bottom
                    visible: pageButton.active
                    color: "#f0008c"  // --secondary
                }
            }
        }
    }

    // Output state, right-aligned where the reference keeps its output
    // controls. The colour is the reference's connected/disconnected pair.
    Row {
        anchors.right: parent.right
        anchors.rightMargin: 14
        anchors.verticalCenter: parent.verticalCenter
        spacing: 8

        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: 8
            height: 8
            radius: 4
            color: worker.outputLive ? "#27a827" : "#a82727"
        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            color: worker.outputLive ? "#27a827" : "#a82727"
            font.pixelSize: 12
            text: worker.status
        }
    }
}
