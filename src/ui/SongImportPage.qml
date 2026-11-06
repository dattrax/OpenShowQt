import QtQuick
import QtQuick.Controls

/*
 * The clipboard song import page (tasks SI.3/SI.4).
 *
 * Paste replaces the draft; the editable title and attribution are separate
 * from the words; the generated slides are worker pictures, grouped by the
 * stanza they came from. There is exactly one destination — Library → Songs —
 * and no canvas duplicate, presenter key or service insertion on this page.
 * A theme change re-measures from the held text without rereading the
 * clipboard; a hidden page stops asking for thumbnails.
 */
ScrollView {
    id: songPage
    objectName: "songImportPage"
    contentWidth: availableWidth
    clip: true
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

    signal closeRequested()

    Connections {
        target: docs
        function onDocumentsChanged() {
            if (songPage.visible)
                songImport.regenerate()
        }
    }

    function askPlanThumbnails() {
        if (!songPage.visible)
            return
        const slides = songImport.slides
        if (slides.length === 0) {
            worker.clearThumbnails()
            return
        }
        const w = Math.max(32, Math.min(512, Math.round(songCells.cellWidth - 18)))
        const asks = []
        const seen = ({})
        const wanted = songCells.visibleIndexes()
        for (const i of wanted) {
            const key = songImport.previewKey(i)
            if (seen[key])
                continue
            const scene = songImport.previewSceneJson(i)
            if (scene === "")
                continue
            seen[key] = true
            asks.push({key: key, scene: scene})
        }
        if (asks.length === 0) {
            worker.clearThumbnails()
            return
        }
        worker.requestThumbnailWindow(asks, w, Math.round(w * 9 / 16))
    }
    Timer {
        id: songAsk
        interval: 80
        onTriggered: songPage.askPlanThumbnails()
    }
    Connections {
        target: songImport
        function onSlidesChanged() { songAsk.restart() }
        function onStateChanged() { if (songImport.slides.length > 0) songAsk.restart() }
    }
    onVisibleChanged: {
        if (visible) {
            songAsk.restart()
            songImport.regenerate()
        } else {
            songAsk.stop()
            worker.clearThumbnails()
        }
    }
    Component.onCompleted: songImport.previewActive = false

    Column {
        width: songPage.availableWidth
        spacing: 10

        Row {
            width: parent.width
            spacing: 8
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "Song Import"
                color: "#eeeeee"; font.pixelSize: 18; font.bold: true
            }
            Item { width: Math.max(0, parent.width - 470); height: 1 }
            Button {
                objectName: "songPasteButton"
                text: "Paste from clipboard"
                onClicked: songImport.pasteFromClipboard()
            }
            Button {
                objectName: "songImportBackButton"
                text: "Back"
                onClicked: songPage.closeRequested()
            }
        }

        Text {
            objectName: "songImportStatusLabel"
            width: parent.width
            wrapMode: Text.WordWrap
            color: songImport.canAdd ? "#8ec9f5" : "#d8c07a"
            font.pixelSize: 12
            text: songImport.status
        }

        Grid {
            width: parent.width
            columns: width >= 900 ? 2 : 1
            columnSpacing: 10
            rowSpacing: 6
            TextField {
                id: titleField
                objectName: "songTitleField"
                width: Math.min(songPage.availableWidth - 20,
                                songPage.availableWidth >= 900 ? (songPage.availableWidth - 30) / 2
                                                               : songPage.availableWidth - 20)
                placeholderText: "Song title"
                text: songImport.title
                onTextChanged: if (text !== songImport.title) songImport.title = text
                color: "#eeeeee"; font.pixelSize: 14
                Accessible.name: "Song title"
            }
            TextField {
                id: ccliField
                objectName: "songCcliField"
                width: titleField.width
                placeholderText: "CCLI song number (digits)"
                text: songImport.ccli
                onTextChanged: if (text !== songImport.ccli) songImport.ccli = text
                color: "#eeeeee"; font.pixelSize: 13
                Accessible.name: "CCLI song number"
            }
            TextField {
                id: authorsField
                objectName: "songAuthorsField"
                width: titleField.width
                placeholderText: "Author / copyright holder"
                text: songImport.authors
                onTextChanged: if (text !== songImport.authors) songImport.authors = text
                color: "#eeeeee"; font.pixelSize: 13
                Accessible.name: "Author or copyright holder"
            }
            TextArea {
                id: copyrightField
                objectName: "songCopyrightField"
                width: titleField.width
                height: 64
                wrapMode: TextArea.Wrap
                placeholderText: "Copyright / licence block"
                text: songImport.copyright
                onTextChanged: if (text !== songImport.copyright) songImport.copyright = text
                color: "#eeeeee"; font.pixelSize: 12
                Accessible.name: "Copyright and licence"
            }
        }

        Row {
            spacing: 8
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "GENERATED SLIDES"
                color: "#bbbbbb"; font.pixelSize: 11; font.bold: true
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                visible: songImport.measuring
                text: "measuring with the audience's renderer…"
                color: "#8ec9f5"; font.pixelSize: 11
            }
        }
        SlidePreviewCollection {
            id: songCells
            objectName: "songSlideCollection"
            width: songPage.availableWidth
            height: Math.min(460, Math.ceil(Math.max(1, songImport.slides.length)
                                            / Math.max(1, columns)) * cellHeight)
            visible: songImport.slides.length > 0
            cellWidth: 210
            grouped: false
            source: songImport.slides
            onVisibleBandChanged: songPage.askPlanThumbnails()
            delegate: SlidePreviewCard {
                required property var modelData
                required property int index
                objectName: "songSlideCard"
                width: songCells.cellWidth - 12
                height: songCells.cellHeight - 12
                caption: (index + 1) + "  " + (modelData.row.group !== ""
                                                ? modelData.row.group
                                                : modelData.row.label)
                Accessible.role: Accessible.StaticText
                Accessible.name: "Generated song slide " + (index + 1) + ": "
                                 + modelData.row.label
                surface.controller: worker
                surface.target: "thumbnail"
                surface.key: songImport.previewKey(index)
                surface.placeholder: modelData.row.lines.join("\n")
                surface.placeholderColor: "#c8c8c8"
                surface.placeholderPixelSize: 11
            }
        }

        Row {
            spacing: 8
            Button {
                objectName: "songAddToLibraryButton"
                text: "Add to Library"
                enabled: songImport.canAdd
                onClicked: {
                    if (songImport.addToLibrary())
                        songPage.closeRequested()
                }
            }
            Button {
                objectName: "songCancelButton"
                text: "Cancel"
                onClicked: songImport.cancel()
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                visible: songImport.hasDraft && !songImport.canAdd
                color: "#b0b0b0"; font.pixelSize: 11
                text: songImport.measuring
                      ? "Add wakes when the slides are measured"
                      : (songImport.title.trim().length === 0
                         ? "Type the song's title to enable Add"
                         : "No measured slides yet")
            }
        }
    }
}
