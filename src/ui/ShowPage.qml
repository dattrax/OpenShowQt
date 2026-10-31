import QtQuick
import QtQuick.Controls

// Presenting only. Slide text, authoring commands and history live in EditPage.
Rectangle {
    id: showPage
    color: "#202020"
    property bool following: false
    signal presentRequested()
    signal editRequested()
    readonly property string showName: docs.songs.filter(s => s.id === docs.currentSongId)
                                                       .map(s => s.name).join("")

    // Request the visible rows plus one ahead, within the worker's 32-slide cap.
    // Scrolling, resizing and keyboard navigation all move this band.
    function requestSlideBand() {
        if (!visible || !worker.thumbnailSupported) return
        const slides = docs.slides
        if (slides.length === 0) { worker.clearThumbnails(); return }
        const first = Math.max(0, Math.floor(slideGrid.contentY / slideGrid.cellHeight))
                      * slideGrid.columns
        const count = Math.min(32, (Math.ceil(slideGrid.height / slideGrid.cellHeight) + 1)
                                  * slideGrid.columns)
        const band = []
        for (let i = first; i < Math.min(slides.length, first + count); ++i)
            band.push({key: slides[i].id, scene: docs.sceneJsonForSlide(slides[i].id)})
        worker.requestThumbnails(band, 320, 180)
    }
    Timer { id: thumbnailAsk; interval: 80; onTriggered: showPage.requestSlideBand() }
    Connections {
        target: docs
        function onNavigationChanged() {
            slideGrid.positionViewAtIndex(docs.currentSlide, GridView.Contain)
            thumbnailAsk.restart()
        }
        function onDocumentsChanged() { thumbnailAsk.restart() }
        function onSlideTextChanged() { thumbnailAsk.restart() }
    }
    property bool bandAskedFor: false
    Connections {
        target: worker
        function onPreviewChanged() {
            if (worker.thumbnailSupported && !showPage.bandAskedFor) {
                showPage.bandAskedFor = true
                thumbnailAsk.restart()
            }
        }
    }
    onVisibleChanged: if (visible) thumbnailAsk.restart()
    Component.onCompleted: thumbnailAsk.restart()

    Rectangle {
        id: heading
        anchors.top: parent.top
        width: parent.width; height: 38
        color: "#141414"
        Text {
            anchors.left: parent.left; anchors.leftMargin: 14
            anchors.right: slideCount.left; anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            text: showPage.showName || "Select a presentation"
            color: "#eeeeee"; font.pixelSize: 14; font.bold: true
            elide: Text.ElideRight
        }
        Text {
            id: slideCount
            anchors.right: parent.right; anchors.rightMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            color: "#aaaaaa"; font.pixelSize: 11
            text: docs.slides.length + (docs.slides.length === 1 ? " slide" : " slides")
        }
    }

    GridView {
        id: slideGrid
        objectName: "operatorSlideGrid"
        anchors.top: heading.bottom
        anchors.bottom: transport.top
        anchors.left: parent.left; anchors.right: parent.right
        anchors.margins: 12
        clip: true
        readonly property int columns: Math.max(1, Math.floor(width / 190))
        cellWidth: width / columns
        cellHeight: (cellWidth - 12) * 9 / 16 + 48
        model: docs.slides
        onContentYChanged: thumbnailAsk.restart()
        onWidthChanged: thumbnailAsk.restart()
        onHeightChanged: thumbnailAsk.restart()
        ScrollBar.vertical: ScrollBar {}
        delegate: AbstractButton {
            id: tile
            required property var modelData
            width: slideGrid.cellWidth - 12
            height: slideGrid.cellHeight - 12
            focusPolicy: Qt.NoFocus
            readonly property bool selected: modelData.index === docs.currentSlide
            Accessible.name: "Slide " + (modelData.index + 1) + ": " + modelData.preview
            onClicked: {
                slideGrid.forceActiveFocus()
                docs.selectSlide(modelData.index)
                if (showPage.following) showPage.presentRequested()
            }
            background: Rectangle {
                color: tile.hovered ? "#393939" : "#2d2d2d"
                border.width: tile.selected ? 2 : 1
                border.color: tile.selected ? "#1685f8" : "#505050"
            }
            contentItem: Item {
                Rectangle {
                    id: picture
                    anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
                    anchors.margins: 3
                    height: width * 9 / 16
                    color: "black"
                    Text {
                        anchors.fill: parent; anchors.margins: 12
                        visible: !worker.thumbnailSupported
                        text: tile.modelData.preview
                        color: "#eeeeee"; font.pixelSize: 13
                        wrapMode: Text.WordWrap
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    PreviewView {
                        visible: worker.thumbnailSupported
                        anchors.fill: parent
                        controller: worker
                        target: PreviewView.Thumbnail
                        key: tile.modelData.id
                    }
                }
                Rectangle {
                    anchors.top: picture.bottom; anchors.bottom: parent.bottom
                    anchors.left: parent.left; anchors.right: parent.right
                    anchors.margins: 2
                    color: tile.selected ? "#1678cf" : "#414141"
                    Text {
                        anchors.fill: parent; anchors.leftMargin: 6; anchors.rightMargin: 6
                        verticalAlignment: Text.AlignVCenter
                        text: (tile.modelData.index + 1) + "  " + tile.modelData.preview
                        color: "#eeeeee"; font.pixelSize: 12
                        elide: Text.ElideRight
                    }
                }
            }
        }
    }
    Column {
        anchors.centerIn: slideGrid
        width: Math.max(0, slideGrid.width - 40)
        spacing: 10
        visible: docs.slides.length === 0
        Text {
            width: parent.width; horizontalAlignment: Text.AlignHCenter
            color: "#dddddd"; font.pixelSize: 18
            text: docs.currentSongId === "" ? "Choose a presentation" : "This presentation has no slides"
        }
        Text {
            width: parent.width; horizontalAlignment: Text.AlignHCenter
            color: "#aaaaaa"; font.pixelSize: 12; wrapMode: Text.WordWrap
            text: docs.currentSongId === "" ? "Select an item in the library or running order."
                                          : "Open Edit to add slides and prepare your content."
        }
    }
    Rectangle {
        id: transport
        anchors.bottom: parent.bottom
        width: parent.width; height: 52
        color: "#2c2c2c"
        Row {
            anchors.left: parent.left; anchors.leftMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            spacing: 6
            PanelButton {
                text: "\u25c0"
                Accessible.name: "Previous slide"
                enabled: docs.slides.length > 0
                onClicked: {
                    slideGrid.forceActiveFocus()
                    docs.stepSlide(-1)
                    if (showPage.following) showPage.presentRequested()
                }
            }
            PanelButton {
                text: "\u25b6"
                Accessible.name: "Next slide"
                enabled: docs.slides.length > 0
                onClicked: {
                    slideGrid.forceActiveFocus()
                    docs.stepSlide(1)
                    if (showPage.following) showPage.presentRequested()
                }
            }
            PanelButton {
                text: "Present"
                highlighted: true
                enabled: docs.slides.length > 0 && worker.outputLive
                onClicked: { slideGrid.forceActiveFocus(); showPage.following = true; showPage.presentRequested() }
            }
            CheckBox {
                text: "Follow selection"
                font.pixelSize: 11
                checked: showPage.following
                onToggled: showPage.following = checked
            }
        }
        PanelButton {
            anchors.right: parent.right; anchors.rightMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            text: "Edit"
            visible: showPage.width >= 530
            enabled: docs.currentSongId !== ""
            onClicked: showPage.editRequested()
        }
    }
}
