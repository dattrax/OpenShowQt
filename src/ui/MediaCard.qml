import QtQuick
import QtQuick.Controls

/*
 * UI part: MediaCard; contract: docs/ui/control-sets.md#mediacard
 *
 * One media asset as a preview card: the picture where the file has one, a
 * kind banner where it does not, the name, and the one notice worth carrying.
 * The bin's grid and (in MB.8) the presentation library's deck pages wear it.
 *
 * The card is a selection target and a drag-source host, never the owner of a
 * document verb: `activated` and the host's drop handling decide what a click
 * or a drop MEANS. Artwork is drawn here rather than shipped as per-kind
 * bitmaps, so the camera colours and numerals stay one component (MB.5).
 */
Item {
    id: card

    required property var modelData
    property string kind: modelData.kind || ""
    property string kindLabel: ""
    property string notice: ""
    property string sourcePath: modelData.sourcePath || ""
    property bool selected: false
    // Camera cards wear 1..4; the host numbers them in creation order.
    property int cameraNumber: 0

    signal activated()

    readonly property bool hasStill: kind === "picture" && sourcePath !== ""

    Rectangle {
        anchors.fill: parent
        color: card.selected ? "#1d3b5c"
             : (cardMouse.containsMouse ? "#3a3a3a" : "#242424")
        border.width: card.selected ? 2 : 1
        border.color: card.selected ? "#1685f8" : "#4a4a4a"
        radius: 2

        Item {
            id: preview
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.topMargin: 4
            anchors.leftMargin: 4
            anchors.rightMargin: 4
            height: Math.round(width * 9 / 16)
            clip: true

            Image {
                id: still
                anchors.fill: parent
                visible: card.hasStill
                source: card.hasStill ? ("file://" + card.sourcePath) : ""
                fillMode: Image.PreserveAspectFit
                asynchronous: true
                cache: true
                smooth: true
            }

            // Camera artwork: a neutral glyph tinted per source, with the
            // numeral over it. No NDI mark (MB.5).
            Item {
                anchors.centerIn: parent
                visible: card.kind === "camera"
                width: Math.min(parent.width, parent.height)
                height: width
                Rectangle {
                    anchors.fill: parent
                    radius: 4
                    color: ["#2f5f8f", "#2f7f5f", "#8f5f2f", "#6f3f8f"][
                        Math.max(0, Math.min(3, card.cameraNumber - 1))]
                }
                Text {
                    anchors.centerIn: parent
                    text: card.cameraNumber > 0 ? card.cameraNumber : ""
                    color: "#ffffff"
                    font.pixelSize: Math.round(parent.height * 0.42)
                    font.bold: true
                }
            }

            Text {
                anchors.centerIn: parent
                visible: !still.visible && card.kind !== "camera"
                text: card.kind === "video" ? "VIDEO"
                      : card.kind === "audio" ? "AUDIO"
                      : card.kind === "other" ? "OTHER" : ""
                color: "#9fb2c4"
                font.pixelSize: 13
                font.bold: true
                font.letterSpacing: 1
            }
        }

        Text {
            id: kindText
            objectName: "mediaRowKind"
            anchors.top: preview.bottom
            anchors.right: parent.right
            anchors.rightMargin: 6
            anchors.topMargin: 4
            font.pixelSize: 10
            color: "#8fb7dd"
            text: card.kindLabel
        }
        Text {
            id: nameText
            objectName: "mediaRowName"
            anchors.top: preview.bottom
            anchors.left: parent.left
            anchors.leftMargin: 6
            anchors.right: kindText.left
            anchors.rightMargin: 6
            anchors.topMargin: 4
            elide: Text.ElideMiddle
            font.pixelSize: 12
            color: "#eeeeee"
            text: modelData.name || ""
        }
        Text {
            objectName: "mediaRowNotice"
            anchors.top: nameText.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: 6
            anchors.rightMargin: 6
            anchors.topMargin: 2
            visible: card.notice !== ""
            elide: Text.ElideRight
            font.pixelSize: 10
            color: "#c8a24a"
            text: card.notice
        }
    }

    MouseArea {
        id: cardMouse
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton
        onClicked: card.activated()
    }
}
