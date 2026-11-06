import QtQuick
import QtQuick.Controls

// Slide navigation for the selected presentation. Pictures use the same
// bounded worker-thumbnail lane as Show and Themes; clicks only move the
// editing cursor, never the audience output.
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
        for (let i = first; i < end; ++i)
            batch.push({key: rows[i].id, scene: docs.sceneJsonForSlide(rows[i].id)})
        return batch
    }
    function requestBand() {
        if (!rail.visible || !worker.thumbnailSupported) return
        const batch = thumbnailWindow()
        if (!batch.length) { worker.clearThumbnails(); return }
        const width = Math.max(160, Math.min(512, Math.round(slides.width - 6)))
        ++rail.bandSent
        worker.requestThumbnailWindow(batch, width, Math.round(width * 9 / 16))
    }
    Timer { id: thumbnailAsk; interval: 80; onTriggered: rail.requestBand() }
    Connections {
        target: docs
        function onDocumentsChanged() {
            ++rail.revisionTick
            if (rail.visible) { rail.revealSelection(); thumbnailAsk.restart() }
        }
        function onNavigationChanged() {
            if (rail.visible) { rail.revealSelection(); thumbnailAsk.restart() }
        }
    }
    Connections {
        target: worker
        function onPreviewChanged() {
            if (!worker.thumbnailSupported) rail.askedFor = false
            if (rail.visible && worker.thumbnailSupported && !rail.askedFor) {
                rail.askedFor = true
                thumbnailAsk.restart()
            }
        }
    }
    onVisibleChanged: {
        if (visible) { rail.revealSelection(); thumbnailAsk.restart() }
        else {
            thumbnailAsk.stop()
            worker.clearThumbnails()
            rail.askedFor = false
        }
    }
    Keys.onPressed: event => {
        if (event.key === Qt.Key_Up || event.key === Qt.Key_Down) {
            rail.selectSlide(Math.max(0, Math.min(rows.length - 1,
                docs.currentSlide + (event.key === Qt.Key_Down ? 1 : -1))))
            event.accepted = true
        } else if (event.key === Qt.Key_Home || event.key === Qt.Key_End) {
            rail.selectSlide(event.key === Qt.Key_Home ? 0 : rows.length - 1)
            event.accepted = true
        }
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
        readonly property real rowHeight: Math.round((width - 6) * 9 / 16) + 40
        onContentYChanged: if (rail.visible) thumbnailAsk.restart()
        onWidthChanged: if (rail.visible) { rail.revealSelection(); thumbnailAsk.restart() }
        onHeightChanged: if (rail.visible) { rail.revealSelection(); thumbnailAsk.restart() }
        ScrollBar.vertical: ScrollBar {}
        delegate: SlidePreviewCard {
            required property var modelData
            required property int index
            objectName: "editorSlideTile"
            width: slides.width
            height: slides.rowHeight - 8
            selected: index === docs.currentSlide
            caption: (index + 1) + "  " + modelData.preview
            sectionLabel: modelData.sectionLabel || ""
            showSectionLabel: index === 0 || modelData.sectionLabel.trim().toLowerCase()
                !== rail.rows[index - 1].sectionLabel.trim().toLowerCase()
            surface.controller: worker
            surface.target: "thumbnail"
            surface.key: modelData.id
            surface.pixelsVisible: rail.visible && worker.thumbnailSupported
            surface.placeholderVisible: !worker.thumbnailSupported
            surface.placeholder: modelData.preview
            surface.placeholderColor: "#eeeeee"
            onActivated: rail.selectSlide(index)
            Accessible.name: "Edit slide " + (index + 1) + ": " + modelData.preview
            Accessible.role: Accessible.Button
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
            text: "Add slide"
            enabled: docs.currentSongId !== ""
            onClicked: { docs.addSlide(); rail.forceActiveFocus() }
        }
    }
}
