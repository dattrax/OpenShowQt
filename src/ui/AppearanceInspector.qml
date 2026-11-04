import QtQuick

/*
 * UI part: AppearanceInspector; contract: docs/ui/control-sets.md#appearanceinspector
 *
 * The shared inspector rail (task CS.5): one adapter target, composed
 * sections. Every section reads and writes the SAME field shape through
 * the InspectorAdapter, so the Songs rail (a theme), the Bible rail (a
 * text context), the Edit rail (a slide, CS.6) and Settings' movement
 * section (the service) are one set of parts wearing different names —
 * not four dialects of the same questions.
 *
 * Section order is the rail's: colours, type, box, effects, movement.
 * The host hides what its target does not answer for (a text context has
 * no colours and no effects until TV.3, so the Bible rail shows neither),
 * passes the automation names its tests read, and owns page-specific
 * rows above and below (names, toggles, measurement notes) — this file
 * composes, it does not gatekeep.
 *
 * Display updates ride the adapter's changed()/targetChanged() into each
 * section's own sync(); call syncAll() after a host-side write that the
 * adapter did not route.
 */
Column {
    id: inspector

    required property InspectorAdapter adapter
    required property var worker           // the family list source
    // The page a centred popup (the font picker) belongs to.
    property Item overlayParent: null

    property bool showBackground: true
    property bool showStyle: true
    property bool showGeometry: true
    property bool showEffects: true
    property bool showTransition: true

    readonly property alias backgroundControls: backgroundControls
    readonly property alias styleControls: styleControls
    readonly property alias geometryControls: geometryControls
    readonly property alias effectsControls: effectsControls
    readonly property alias transitionControls: transitionControls

    function syncAll() {
        backgroundControls.sync()
        styleControls.sync()
        geometryControls.sync()
        effectsControls.sync()
        transitionControls.sync()
    }

    spacing: 8
    width: parent ? parent.width : 0

    BackgroundControls {
        id: backgroundControls
        objectName: "backgroundControls"
        visible: inspector.showBackground
        width: parent.width
        adapter: inspector.adapter
    }
    TextStyleControls {
        id: styleControls
        objectName: "textStyleControls"
        visible: inspector.showStyle
        width: parent.width
        adapter: inspector.adapter
        worker: inspector.worker
        overlayParent: inspector.overlayParent
    }
    BoxGeometryControls {
        id: geometryControls
        objectName: "boxGeometryControls"
        visible: inspector.showGeometry
        width: parent.width
        adapter: inspector.adapter
    }
    TextEffectsControls {
        id: effectsControls
        objectName: "textEffectsControls"
        visible: inspector.showEffects
        width: parent.width
        adapter: inspector.adapter
    }
    TransitionControls {
        id: transitionControls
        objectName: "transitionControls"
        visible: inspector.showTransition
        width: parent.width
        adapter: inspector.adapter
    }
}
