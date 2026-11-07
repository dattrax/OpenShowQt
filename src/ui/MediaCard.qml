import QtQuick
import QtQuick.Controls
import QtQuick.Window

/*
 * UI part: MediaCard; contract: docs/ui/control-sets.md#mediacard
 *
 * One media asset as a preview card: the picture where the file has one, a
 * kind banner where it does not, the name, and the one notice worth carrying.
 * The bin's grid wears it; the Library's row cards wear the same CardChrome.
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

    // The card's drag payload, exposed so a host/test can read what a drag
    // would carry without reaching into the overlay-parented proxy.
    readonly property string dragPayload: "media:" + (modelData.id || "")

    // Ask once for this card's video. The worker holds the want while it is
    // not yet connected (and replays it after a reconnect), so a card built
    // before the handshake still gets its picture. A GridView reuses
    // delegates, so a card can become another file without being completed
    // again — the property handlers cover that case too.
    function requestThumbnail() {
        if (kind === "video" && sourcePath !== "")
            worker.requestVideoThumbnail(sourcePath)
    }
    Component.onCompleted: requestThumbnail()
    onSourcePathChanged: requestThumbnail()
    onKindChanged: requestThumbnail()

    signal activated()
    // A right-click in card coordinates; the host owns the menu.
    signal contextRequested(real mouseX, real mouseY)

    readonly property bool hasStill: kind === "picture" && sourcePath !== ""
    // MB.6: the worker's first frame as a data URI, or "" for the glyph.
    readonly property string videoThumb: kind === "video" && sourcePath !== ""
        ? (worker.videoThumbnails[sourcePath] || "") : ""

    CardChrome {
        anchors.fill: parent
        selected: card.selected
        hovered: cardMouse.containsMouse

        Item {
            id: preview
            anchors.fill: parent
            anchors.margins: 4
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

            // Camera artwork: our own neutral silhouette, tinted per source
            // and numbered 1..4. No NDI mark (MB.5).
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
                Item {
                    anchors.centerIn: parent
                    width: parent.width * 0.66
                    height: width * 0.72
                    Rectangle {
                        anchors.fill: parent
                        radius: 3
                        color: "#ffffff"
                    }
                    Rectangle {  // viewfinder hump
                        x: parent.width * 0.16
                        y: -parent.height * 0.26
                        width: parent.width * 0.38
                        height: parent.height * 0.30
                        radius: 2
                        color: "#ffffff"
                    }
                    Rectangle {  // lens barrel
                        anchors.centerIn: parent
                        width: parent.height * 0.62
                        height: width
                        radius: width / 2
                        color: "#202020"
                    }
                    Rectangle {  // lens
                        anchors.centerIn: parent
                        width: parent.width * 0.32
                        height: width
                        radius: width / 2
                        color: "#ffffff"
                    }
                }
                Text {
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.margins: 3
                    text: card.cameraNumber > 0 ? card.cameraNumber : ""
                    color: "#ffffff"
                    font.pixelSize: Math.max(9, Math.round(parent.height * 0.20))
                    font.bold: true
                }
            }

            // Audio: a record/turntable glyph, not a note.
            Item {
                anchors.centerIn: parent
                visible: card.kind === "audio"
                width: Math.min(parent.width, parent.height) * 0.8
                height: width
                Rectangle {
                    anchors.fill: parent
                    radius: width / 2
                    color: "#dfe7ee"
                }
                Rectangle {
                    anchors.centerIn: parent
                    width: parent.width * 0.72
                    height: width
                    radius: width / 2
                    color: "#2a2a2a"
                }
                Rectangle {
                    anchors.centerIn: parent
                    width: parent.width * 0.18
                    height: width
                    radius: width / 2
                    color: "#dfe7ee"
                }
                Rectangle {
                    anchors.centerIn: parent
                    width: parent.width * 0.06
                    height: width
                    radius: width / 2
                    color: "#2a2a2a"
                }
                Rectangle {  // tonearm
                    x: parent.width * 0.70
                    y: parent.height * 0.06
                    width: Math.max(2, parent.width * 0.055)
                    height: parent.height * 0.52
                    color: "#dfe7ee"
                    transform: Rotation {
                        origin.x: width / 2
                        origin.y: 0
                        angle: 28
                    }
                }
            }

            // MB.6: the first frame, when the worker has answered.
            Image {
                id: videoFrame
                anchors.fill: parent
                visible: card.videoThumb !== ""
                source: card.videoThumb
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
                cache: true
                smooth: true
            }

            // Video: a play mark while the first frame is not here yet.
            Text {
                anchors.centerIn: parent
                visible: card.kind === "video" && card.videoThumb === ""
                text: "\u25B6"
                color: "#dfe7ee"
                font.pixelSize: Math.round(parent.height * 0.55)
            }

            // Other: a plain document mark.
            Item {
                anchors.centerIn: parent
                visible: card.kind === "other"
                width: Math.min(parent.width, parent.height) * 0.45
                height: width * 1.25
                Rectangle {
                    anchors.fill: parent
                    radius: 2
                    color: "#dfe7ee"
                }
                Repeater {
                    model: [0.32, 0.50, 0.68]
                    delegate: Rectangle {
                        x: parent.width * 0.18
                        y: parent.height * modelData
                        width: parent.width * 0.64
                        height: 2
                        color: "#2a2a2a"
                    }
                }
            }
        }

        // The name is in the inspector, so the card is the preview. Kind and
        // notice stay as small overlays so the picture keeps the space.
        Rectangle {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.margins: 6
            width: kindText.width + 8
            height: kindText.height + 4
            radius: 2
            color: "#aa000000"
            Text {
                id: kindText
                objectName: "mediaRowKind"
                anchors.centerIn: parent
                font.pixelSize: 10
                color: "#8fb7dd"
                text: card.kindLabel
            }
        }
        Rectangle {
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.margins: 6
            visible: card.notice !== ""
            width: noticeText.width + 8
            height: noticeText.height + 4
            radius: 2
            color: "#aa000000"
            Text {
                id: noticeText
                objectName: "mediaRowNotice"
                anchors.centerIn: parent
                font.pixelSize: 10
                color: "#c8a24a"
                text: card.notice
            }
        }
    }

    // The card is a drag source: the payload names the asset, never a document
    // verb. A drop target decides what placing it means (MB.3).
    DragFeedback {
        id: cardDrag
        objectName: "mediaDragProxy"
        payload: card.dragPayload
        host: card.Window.window ? card.Window.window.contentItem : null
        title: modelData.name || ""
        hint: "Drop on a cue to set its background"
        dragging: cardMouse.drag.active
    }

    MouseArea {
        id: cardMouse
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        // A click selects; a drag carries the card. The flag keeps a completed
        // drag from also selecting on release.
        property bool dragMoved: false
        drag.smoothed: false
        drag.target: cardDrag
        onPressed: function (mouse) {
            dragMoved = false
            if (mouse.button === Qt.LeftButton) cardDrag.begin(card, mouse.x, mouse.y)
        }
        onPositionChanged: {
            if (drag.active) dragMoved = true
        }
        onReleased: {
            if (cardDrag.Drag.active) cardDrag.Drag.drop()
            cardDrag.x = 0
            cardDrag.y = 0
        }
        onCanceled: {
            cardDrag.x = 0
            cardDrag.y = 0
        }
        onClicked: function (mouse) {
            if (mouse.button === Qt.RightButton) {
                card.contextRequested(mouse.x, mouse.y)
            } else if (!dragMoved) {
                card.activated()
            }
        }
    }
}
