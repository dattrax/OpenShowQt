import QtQuick

/*
 * UI part: SlidePreviewSurface; contract: docs/ui/control-sets.md#slidepreviewsurface
 *
 * One aspect-fit worker picture in an honest frame (task CS.2). This is the
 * single spelling of what every preview pane in the workspace has kept
 * re-inventing: a black frame, the worker's own pixels letterboxed inside it,
 * a placeholder that names WHY there are no pixels rather than dressing the
 * emptiness up as content, and an overlay slot pinned to the fitted content
 * rectangle so selection outlines and handles ride the picture through a
 * resize instead of drifting over the letterbox.
 *
 * Boundaries the contract is strict about:
 *
 *   - The surface DISPLAYS. It never schedules a scene ask, never clears a
 *     target and never presents; the owning page decides which scene the
 *     target describes and what a click on the card around it means.
 *   - `PreviewView` stays the only pixel painter. The surface wraps it, sizes
 *     it and reports its readiness; it paints no frame content of its own and
 *     never substitutes one target's picture for another's.
 *   - Readiness is per item and per target (`ready` follows the image THIS
 *     item painted, including a keyed thumbnail), not a global "some frame
 *     arrived" flag a stale delegate could borrow.
 */
Item {
    id: surface

    // The stream this surface reads and which picture of it to show — bound
    // explicitly by every host. `target` is the contract vocabulary
    // ("audience" | "selection" | "thumbnail"); `key` is the thumbnail's
    // identity and is ignored by the other two.
    property var controller: null
    property string target: "audience"
    property string key: ""

    // The picture rule. Every ask on today's wire is 16:9, so this is a
    // property rather than arithmetic scattered through four pages.
    property real aspectRatio: 16.0 / 9.0

    // Frame chrome. Hosts keep the borders their screens are known by.
    property color frameColor: "#000000"
    property color borderColor: "#4a4a4a"
    property int borderWidth: 1

    // The honest placeholder. Hosts say what the absence means; the surface
    // only ever shows it while it has no pixels of its own (or while the host
    // overrides `placeholderVisible` for a capability statement — "this
    // worker cannot draw thumbnails" outranks "a frame is on its way").
    property string placeholder: ""
    property bool placeholderVisible: placeholder !== "" && !ready
    property color placeholderColor: "#888888"
    property int placeholderPixelSize: 12
    // Automation name for the note, for hosts whose tests read it.
    property string placeholderObjectName: ""

    // Whether the pixels themselves may show. A page that only has a picture
    // for a slide it actually holds says so here; the frame stays, honest
    // black, underneath.
    property bool pixelsVisible: true
    // Overlay children clip to the content rect (the Themes placement box
    // wants this; the Edit canvas handles would rather not be cut).
    property bool clipOverlays: false

    // What the contract owes the overlays and the tests.
    readonly property alias preview: view
    readonly property bool ready: view.hasFrame
    readonly property real innerW: Math.max(0, width - 2 * borderWidth)
    readonly property real innerH: Math.max(0, height - 2 * borderWidth)
    readonly property real fitW: innerH * aspectRatio <= innerW ? innerH * aspectRatio
                                                                : innerW
    readonly property real fitH: fitW / aspectRatio
    // The rect the pixels actually occupy, in this item's coordinates.
    readonly property rect contentRect: Qt.rect(borderWidth + (innerW - fitW) / 2,
                                                borderWidth + (innerH - fitH) / 2,
                                                fitW, fitH)
    // The authored-unit scale, so overlays use ONE number for 1920x1080.
    readonly property real zoom: contentRect.width / 1920

    // The overlay slot: children land above the pixels, pinned to
    // `contentRect`, never below the black.
    default property alias overlay: overlaySlot.data

    Rectangle {
        anchors.fill: parent
        color: surface.frameColor
        border.width: surface.borderWidth
        border.color: surface.borderColor
    }

    PreviewView {
        id: view
        x: surface.borderWidth
        y: surface.borderWidth
        width: surface.innerW
        height: surface.innerH
        controller: surface.controller
        visible: surface.pixelsVisible
        target: surface.target === "selection" ? PreviewView.Selection
                : (surface.target === "thumbnail" ? PreviewView.Thumbnail
                                                  : PreviewView.Audience)
        key: surface.key
    }

    // The placeholder rides the same rectangle as the pixels it stands in for.
    Item {
        x: surface.contentRect.x
        y: surface.contentRect.y
        width: surface.contentRect.width
        height: surface.contentRect.height
        visible: surface.placeholderVisible
        Text {
            anchors.fill: parent
            anchors.margins: 8
            objectName: surface.placeholderObjectName
            color: surface.placeholderColor
            font.pixelSize: surface.placeholderPixelSize
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            wrapMode: Text.WordWrap
            elide: Text.ElideRight
            maximumLineCount: 3
            text: surface.placeholder
        }
    }

    Item {
        id: overlaySlot
        x: surface.contentRect.x
        y: surface.contentRect.y
        width: surface.contentRect.width
        height: surface.contentRect.height
        clip: surface.clipOverlays
    }
}
