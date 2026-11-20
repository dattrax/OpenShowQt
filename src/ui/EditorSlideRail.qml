import QtQuick
import QtQuick.Controls

// Slide navigation for the selected presentation. Pictures use the same
// bounded worker-thumbnail lane as Show and Themes; clicks only move the
// editing cursor, never the audience output.
//
// EW.7/EW.8: the rail is also where slides are reordered and inserted. Each
// card carries a visible grab handle; dragging it shows a ghost and an
// insertion gap, and dropping converts that gap to the domain's final-slot
// rule exactly once. The row menu and the rail's own keys offer the same
// moves without a pointer, and the footer's Insert slide lands one fresh
// slide after the selection (position zero when the presentation is empty).
// Nothing here presents: a move or an insert is one document edit.
Rectangle {
    id: rail
    objectName: "editorSlideRail"
    color: "#353535"
    property int revisionTick: 0
    property int bandSent: 0
    property bool askedFor: false
    readonly property var rows: {
        rail.revisionTick
        return docs.slidesOf(docs.currentSongId)
    }
    readonly property string showName: {
        rail.revisionTick
        return docs.showInfo(docs.currentSongId).name || "No presentation selected"
    }

    // --- EW.7 drag state ---------------------------------------------------
    // dragFrom is the source slot captured when the handle was pressed.
    // dragSlideId pins the gesture to that slide's identity, so a model
    // change mid-drag cancels rather than retargeting whatever moved into
    // the slot. dragGap is the VISIBLE insertion gap: 0 before the first
    // card, rows.length after the last.
    property int dragFrom: -1
    property string dragSlideId: ""
    property int dragGap: -1
    property point dragPoint: Qt.point(0, 0)
    property int autoscrollDir: 0
    readonly property bool dragActive: dragFrom >= 0
    // The row menu's target slot; -1 while closed.
    property int menuIndex: -1

    function selectSlide(index) {
        if (index < 0 || index >= rows.length) return
        docs.selectSlide(index)
        rail.forceActiveFocus()
    }
    function revealSelection() {
        if (!rail.visible) return
        Qt.callLater(() => {
            if (rail.visible && docs.currentSlide >= 0)
                slides.positionViewAtIndex(docs.currentSlide, ListView.Contain)
        })
    }
    function thumbnailWindow() {
        const first = Math.max(0, Math.floor(slides.contentY / slides.rowHeight))
        const end = Math.min(rows.length,
            Math.ceil((slides.contentY + slides.height) / slides.rowHeight) + 1)
        const batch = []
        for (let i = first; i < end; ++i) {
            // TC.7: scene + render fingerprint from one compile, so the
            // service's persisted pictures answer before the worker is asked.
            const source = docs.thumbnailSourceForSlide(rows[i].id)
            batch.push({key: rows[i].id, scene: source.scene,
                        fingerprint: source.fingerprint, showId: docs.currentSongId})
        }
        return batch
    }
    function requestBand() {
        if (!rail.visible) return
        if (!worker.thumbnailSupported && !worker.thumbnailCacheAvailable) return
        const batch = thumbnailWindow()
        if (!batch.length) { worker.clearThumbnails(); return }
        const width = Math.max(160, Math.min(512, Math.round(slides.width - 6)))
        ++rail.bandSent
        worker.requestThumbnailWindow(batch, width, Math.round(width * 9 / 16))
    }

    // --- EW.7 gesture helpers ---------------------------------------------
    function beginDrag(index) {
        if (index < 0 || index >= rows.length) return
        rail.dragFrom = index
        rail.dragSlideId = String(rows[index].id)
        rail.dragGap = index
        rail.dragPoint = Qt.point(slides.width / 2, slides.y)
        rail.forceActiveFocus()
    }
    // The gap under a rail-relative pointer, clamped to the legal range.
    function gapForRailY(y) {
        const contentY = slides.contentY + (y - slides.y)
        const gap = Math.round(contentY / slides.rowHeight)
        return Math.max(0, Math.min(rows.length, gap))
    }
    function updateDrag(x, y) {
        if (!rail.dragActive) return
        rail.dragPoint = Qt.point(x, y)
        rail.dragGap = rail.gapForRailY(y)
        // Bounded edge autoscroll: one row per tick while the pointer is
        // within 18 px of the lane's edge.
        if (y < slides.y + 18) rail.autoscrollDir = -1
        else if (y > slides.y + slides.height - 18) rail.autoscrollDir = 1
        else rail.autoscrollDir = 0
    }
    // The final position the visible gap names, in the domain's rule (the
    // destination AFTER removal). The translation happens here, once.
    function dropTarget(gap) {
        return gap > rail.dragFrom ? gap - 1 : gap
    }
    function cancelDrag() {
        rail.dragFrom = -1
        rail.dragSlideId = ""
        rail.dragGap = -1
        rail.autoscrollDir = 0
    }
    function finishDrag(insideLane) {
        if (!rail.dragActive) return
        const from = rail.dragFrom
        const gap = rail.dragGap
        const slideId = rail.dragSlideId
        const to = rail.dropTarget(gap)
        rail.cancelDrag()
        if (!insideLane || from < 0 || gap < 0) return
        if (to === from) return  // the source's own gap: no structural edit
        docs.moveSlideFor(docs.currentSongId, slideId, to)
    }

    function openSlideMenuAt(index, railX, railY) {
        if (index < 0 || index >= rows.length) return
        rail.menuIndex = index
        slideMenu.popup(rail, railX, railY)
    }
    // The row the open menu names, resolved AT CLICK TIME and re-validated:
    // a model change while the menu is open must not address a vanished row.
    function menuSlideId() {
        if (rail.menuIndex < 0 || rail.menuIndex >= rail.rows.length) return ""
        return String(rail.rows[rail.menuIndex].id)
    }
    function menuMove(delta) {
        const id = rail.menuSlideId()
        if (id === "") return
        docs.moveSlideFor(docs.currentSongId, id, rail.menuIndex + delta)
        rail.forceActiveFocus()
    }
    function menuInsert(after) {
        const id = rail.menuSlideId()
        if (id === "") return
        docs.insertSlideFor(docs.currentSongId, id, after)
        rail.forceActiveFocus()
    }
    function openSlideMenuForSelection() {
        if (rows.length === 0 || docs.currentSlide < 0) return
        const y = slides.y + Math.min(slides.height - 8,
            docs.currentSlide * slides.rowHeight - slides.contentY + 20)
        rail.openSlideMenuAt(docs.currentSlide, slides.x + 12, y)
    }
    function moveSelected(delta) {
        docs.moveCurrentSlideStep(delta)
        rail.forceActiveFocus()
    }

    Timer { id: thumbnailAsk; interval: 80; onTriggered: rail.requestBand() }
    // Bounded autoscroll (EW.7): only while a drag is held near an edge, and
    // never more than one row a tick.
    Timer {
        id: edgeScroll
        interval: 50
        repeat: true
        running: rail.dragActive && rail.autoscrollDir !== 0
        onTriggered: {
            if (!rail.dragActive) return
            const step = slides.rowHeight
            const next = slides.contentY + rail.autoscrollDir * step
            const top = 0
            const bottom = Math.max(0, slides.contentHeight - slides.height)
            const landed = Math.max(top, Math.min(bottom, next))
            if (landed === slides.contentY) {
                rail.autoscrollDir = 0
                return
            }
            slides.contentY = landed
            rail.dragGap = rail.gapForRailY(rail.dragPoint.y)
        }
    }
    Connections {
        target: docs
        function onDocumentsChanged() {
            ++rail.revisionTick
            // A move, insert, undo or deletion during a held drag retires
            // the gesture: the captured identity is the authority.
            if (rail.dragActive &&
                (rail.rows.length <= rail.dragFrom ||
                 String(rail.rows[rail.dragFrom].id) !== rail.dragSlideId)) {
                rail.cancelDrag()
            }
            if (rail.visible) { rail.revealSelection(); thumbnailAsk.restart() }
        }
        function onNavigationChanged() {
            if (rail.dragActive) rail.cancelDrag()
            if (rail.visible) { rail.revealSelection(); thumbnailAsk.restart() }
        }
    }
    Connections {
        target: worker
        function onPreviewChanged() {
            if (!worker.thumbnailSupported && !worker.thumbnailCacheAvailable)
                rail.askedFor = false
            if (rail.visible && (worker.thumbnailSupported || worker.thumbnailCacheAvailable)
                    && !rail.askedFor) {
                rail.askedFor = true
                thumbnailAsk.restart()
            }
        }
    }
    onVisibleChanged: {
        if (visible) { rail.revealSelection(); thumbnailAsk.restart() }
        else {
            thumbnailAsk.stop()
            rail.cancelDrag()
            worker.clearThumbnails()
            rail.askedFor = false
        }
    }
    Keys.onPressed: event => {
        if (rail.dragActive) {
            if (event.key === Qt.Key_Escape) {
                rail.cancelDrag()
                event.accepted = true
                return
            }
        }
        if (event.key === Qt.Key_Up || event.key === Qt.Key_Down) {
            const delta = event.key === Qt.Key_Down ? 1 : -1
            if (event.modifiers & Qt.ControlModifier) {
                rail.moveSelected(delta)
            } else {
                rail.selectSlide(Math.max(0, Math.min(rows.length - 1,
                    docs.currentSlide + delta)))
            }
            event.accepted = true
        } else if (event.key === Qt.Key_Home || event.key === Qt.Key_End) {
            rail.selectSlide(event.key === Qt.Key_Home ? 0 : rows.length - 1)
            event.accepted = true
        } else if (event.key === Qt.Key_Insert) {
            rail.insertAfterSelection()
            event.accepted = true
        } else if (event.key === Qt.Key_Menu ||
                   (event.key === Qt.Key_F10 && (event.modifiers & Qt.ShiftModifier))) {
            rail.openSlideMenuForSelection()
            event.accepted = true
        }
    }

    function insertAfterSelection() {
        if (docs.currentSongId === "") return
        const created = docs.insertSlideAfterCurrent()
        rail.forceActiveFocus()
        if (created !== "") rail.revealSelection()
    }

    Column {
        id: heading
        anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
        anchors.margins: 8
        spacing: 4
        Text { text: "SLIDES"; color: "#b0b0b0"; font.pixelSize: 11; font.bold: true }
        Text {
            width: parent.width
            text: rail.showName
            color: "#eeeeee"; font.pixelSize: 13
            elide: Text.ElideRight
        }
    }
    ListView {
        id: slides
        objectName: "editorSlideList"
        anchors.left: parent.left; anchors.right: parent.right
        anchors.top: heading.bottom; anchors.bottom: footer.top
        anchors.margins: 8
        clip: true
        model: rail.rows
        spacing: 8
        currentIndex: docs.currentSlide
        keyNavigationEnabled: false
        // A held grab owns the pointer; the lane never flick-scrolls under it.
        interactive: !rail.dragActive
        readonly property real rowHeight: Math.round((width - 6) * 9 / 16) + 40
        onContentYChanged: if (rail.visible) thumbnailAsk.restart()
        onWidthChanged: if (rail.visible) { rail.revealSelection(); thumbnailAsk.restart() }
        onHeightChanged: if (rail.visible) { rail.revealSelection(); thumbnailAsk.restart() }
        ScrollBar.vertical: ScrollBar {}

        // The insertion gap before/between/after cards, drawn over the lane.
        Rectangle {
            objectName: "editorSlideInsertionGap"
            visible: rail.dragActive && rail.dragGap >= 0
            x: 0
            y: Math.max(-2, Math.min(slides.contentHeight,
                rail.dragGap * slides.rowHeight - 4))
            width: slides.width
            height: 4
            color: "#1685f8"
            z: 20
        }

        delegate: Item {
            id: tileHost
            required property var modelData
            required property int index
            width: slides.width
            height: slides.rowHeight - 8

            SlidePreviewCard {
                id: tileCard
                objectName: "editorSlideTile"
                // The tests and the harness address a tile by its slot; the
                // card is a wrapper of the delegate, so the slot is mirrored.
                property int index: tileHost.index
                anchors.fill: parent
                selected: index === docs.currentSlide
                // The rail heading already names the show, so a slide's strip only
                // carries its own number and its Verse/Chorus marking.
                ordinal: String(index + 1)
                caption: modelData.preview
                sectionLabel: modelData.sectionLabel || ""
                showSectionLabel: index === 0 || modelData.sectionLabel.trim().toLowerCase()
                    !== rail.rows[index - 1].sectionLabel.trim().toLowerCase()
                surface.controller: worker
                surface.target: "thumbnail"
                surface.key: modelData.id
                surface.pixelsVisible: rail.visible && (worker.thumbnailSupported
                                                        || worker.thumbnailCacheAvailable)
                surface.placeholderVisible: !worker.thumbnailSupported
                                            && !worker.thumbnailCacheAvailable
                surface.placeholder: modelData.preview
                surface.placeholderColor: "#eeeeee"
                onActivated: rail.selectSlide(index)
                onContextRequested: function (mouseX, mouseY) {
                    const point = rail.mapFromItem(tileCard, mouseX, mouseY)
                    rail.openSlideMenuAt(index, point.x, point.y)
                }
                Accessible.name: "Edit slide " + (index + 1) + ": " + modelData.preview
                Accessible.role: Accessible.Button
            }

            // EW.7: the visible grab handle. A press here starts a reorder;
            // a press anywhere else still only selects the slide.
            Rectangle {
                id: grabHandle
                objectName: "editorSlideGrabHandle"
                property int index: tileHost.index
                width: 22
                height: 18
                // Clear of the lane's overlay scrollbar: the outer few
                // pixels belong to scrolling, not to grabbing.
                x: tileHost.width - width - 24
                y: 4
                radius: 3
                color: grabMouse.containsMouse || rail.dragActive
                       ? "#1685f8" : "#50000000"
                border.color: "#80ffffff"
                z: 12
                Text {
                    anchors.centerIn: parent
                    text: "\u22ee\u22ee"
                    color: "#ffffff"
                    font.pixelSize: 12
                }
                Accessible.name: "Reorder slide " + (index + 1)
                Accessible.role: Accessible.Button
                MouseArea {
                    id: grabMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.SizeVerCursor
                    preventStealing: true
                    onPressed: rail.beginDrag(index)
                    onPositionChanged: function (mouse) {
                        if (!rail.dragActive) return
                        const point = rail.mapFromItem(grabMouse, mouse.x, mouse.y)
                        rail.updateDrag(point.x, point.y)
                    }
                    onReleased: function (mouse) {
                        const point = rail.mapFromItem(grabMouse, mouse.x, mouse.y)
                        rail.updateDrag(point.x, point.y)
                        rail.finishDrag(point.x >= slides.x && point.x <= slides.x + slides.width
                                        && point.y >= slides.y - 8
                                        && point.y <= slides.y + slides.height + 8)
                    }
                    onCanceled: rail.cancelDrag()
                }
            }
        }
        Text {
            anchors.centerIn: parent
            width: parent.width
            visible: rail.rows.length === 0
            text: "No slides"
            horizontalAlignment: Text.AlignHCenter
            color: "#929292"; font.pixelSize: 13
        }
    }

    // The ghost follows the pointer while a grab is held.
    Rectangle {
        objectName: "editorSlideGhost"
        visible: rail.dragActive
        x: Math.max(0, Math.min(rail.width - width,
            rail.dragPoint.x - width / 2))
        y: Math.max(0, Math.min(rail.height - height, rail.dragPoint.y - height / 2))
        width: Math.max(80, rail.width - 24)
        height: 34
        radius: 3
        color: "#cc202020"
        border.color: "#1685f8"
        z: 40
        Text {
            anchors.fill: parent
            anchors.margins: 6
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
            color: "#eeeeee"
            font.pixelSize: 12
            text: rail.dragFrom >= 0 && rail.dragFrom < rail.rows.length
                  ? rail.rows[rail.dragFrom].preview : ""
        }
    }

    // Right-click / Menu key: the pointer-free move and insert alternatives.
    Menu {
        id: slideMenu
        objectName: "editorSlideMenu"
        MenuItem {
            objectName: "editorMoveUpMenuItem"
            text: "Move up"
            enabled: rail.menuIndex > 0
            onClicked: rail.menuMove(-1)
        }
        MenuItem {
            objectName: "editorMoveDownMenuItem"
            text: "Move down"
            enabled: rail.menuIndex >= 0 && rail.menuIndex < rail.rows.length - 1
            onClicked: rail.menuMove(1)
        }
        MenuSeparator {}
        MenuItem {
            objectName: "editorInsertBeforeMenuItem"
            text: "Insert before"
            enabled: rail.menuIndex >= 0 && rail.menuIndex < rail.rows.length
            onClicked: rail.menuInsert(false)
        }
        MenuItem {
            objectName: "editorInsertAfterMenuItem"
            text: "Insert after"
            enabled: rail.menuIndex >= 0 && rail.menuIndex < rail.rows.length
            onClicked: rail.menuInsert(true)
        }
    }

    Row {
        id: footer
        anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
        anchors.margins: 8
        height: 30
        spacing: 8
        Text {
            width: parent.width - addSlide.width - 8
            height: parent.height
            text: rail.rows.length + (rail.rows.length === 1 ? " slide" : " slides")
            color: "#b0b0b0"; font.pixelSize: 11
            verticalAlignment: Text.AlignVCenter
        }
        PanelButton {
            id: addSlide
            objectName: "editorAddSlide"
            text: "Insert slide"
            enabled: docs.currentSongId !== ""
            onClicked: rail.insertAfterSelection()
        }
    }
}
