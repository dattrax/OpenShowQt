import QtQuick

/*
 * UI part: CameraArtwork (MB.5 artwork, shared by the bin and the service
 * preview). One camera's glyph: our own neutral silhouette, tinted per source
 * and numbered 1..4, with no NDI mark.
 *
 * It is drawn, not shipped as a bitmap, so the bin card, the service cue tile
 * and any later wearer cannot drift apart: an off-air camera has no frame to
 * show, and this is what stands in for it.
 */
Item {
    id: artwork

    // 1..4, the creation-order number the bin gives a camera.
    property int number: 1
    // The four source tints, in creation order, exactly as the bin has always
    // worn them.
    readonly property var tints: ["#2f5f8f", "#2f7f5f", "#8f5f2f", "#6f3f8f"]

    Rectangle {
        anchors.fill: parent
        radius: 4
        color: artwork.tints[Math.max(0, Math.min(3, artwork.number - 1))]
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
        text: artwork.number > 0 ? artwork.number : ""
        color: "#ffffff"
        font.pixelSize: Math.max(9, Math.round(parent.height * 0.20))
        font.bold: true
    }
}
