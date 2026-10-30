import QtQuick
import QtQuick.Controls

/*
 * The show page (task 6.19b6, from Main.qml's 3.3/4.3/6.16b panel): the
 * current song's slide list with the worker's own pictures of each row,
 * the present-on-navigate follow, the history buttons and the lyric
 * editor. Everything here drives DocumentController ("docs"); presenting
 * compiles the selected slide and hands the JSON to the worker, which
 * presents on the acknowledgement.
 *
 * The split's row asked for "select/step/present/text intents to the
 * window"; read honestly, only one of those has a window-side owner.
 * Selecting, stepping and editing call `docs` — the context property —
 * directly, as they always did, and follow mode is this page's own state:
 * nothing outside the block ever read it. What genuinely stays in the
 * window is PRESENTING — presentCurrentSlide() is also what the keyboard
 * executors run — so the page emits exactly one signal: presenting a
 * slide after navigation moved (the Present button, or a row click or step
 * while following) leaves as presentRequested().
 */
Item {
    id: showPage

    // Task 3.3: once a slide has been presented from this panel, navigating
    // follows it onto the output — that is the operator's expectation
    // mid-service. Any Present click (re)arms it.
    property bool following: false

    signal presentRequested()

    Row {
        id: slideActions
        anchors.top: parent.top
        anchors.left: parent.left
        spacing: 8

        Button {
            text: "Add slide"
            enabled: docs.currentSongId !== ""
            onClicked: docs.addSlide()
        }

        Button {
            text: "Remove slide"
            enabled: docs.slides.length > 0
            onClicked: docs.removeCurrentSlide()
        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            color: "#8f93a5"
            font.pixelSize: 12
            text: docs.slides.length > 0
                ? "slide " + (docs.currentSlide + 1) + " of " + docs.slides.length
                : "no slides"
        }
    }

    // The slide list's pictures (task 6.16b, review R16): the worker
    // renders the visible band with the production compositor, so a row
    // shows the slide as the audience would see it — same fonts, same
    // wrap, same background — instead of a line of Qt-laid-out text.
    //
    // The band is the current slide ±5, which is the rows on screen (the
    // box fits about six at 26 px) plus a scroll's worth either way, and
    // it is inside the 32-slide cap the protocol declares. Asking per
    // visible row would send one batch per pixel of scroll; asking for a
    // band moves the list in steps, and the client coalesces a band it
    // already has. A slide the worker has not drawn yet keeps its text
    // line: the fallback is the row's own, so a list is never blank while
    // pictures are on their way.
    function requestSlideBand() {
        if (!worker.thumbnailSupported) {
            return
        }
        const slides = docs.slides
        if (slides.length === 0) {
            worker.clearThumbnails()
            return
        }
        const band = []
        const first = Math.max(0, docs.currentSlide - 5)
        const last = Math.min(slides.length - 1, docs.currentSlide + 5)
        for (let i = first; i <= last; ++i) {
            band.push({key: slides[i].id,
                       scene: docs.sceneJsonForSlide(slides[i].id)})
        }
        worker.requestThumbnails(band, 160, 90)
    }

    // Navigating, editing and editing the document (a slide added, a song
    // switched) all change what the list wants a picture of. The client
    // re-issues across a preview restart and a worker that dropped the
    // batch on its own, so the list does not have to watch the socket too.
    Connections {
        target: docs
        function onNavigationChanged() { showPage.requestSlideBand() }
        function onDocumentsChanged() { showPage.requestSlideBand() }
        function onSlideTextChanged() { showPage.requestSlideBand() }
    }
    // The capability arrives with the handshake, which is usually after
    // this page was built. ONE ask follows the first moment the worker can
    // draw the list: previewChanged fires with every preview frame, and a
    // handler that asked each time would recompile eleven scenes fifteen
    // times a second for a list that already has them.
    property bool bandAskedFor: false
    Connections {
        target: worker
        function onPreviewChanged() {
            if (worker.thumbnailSupported && !showPage.bandAskedFor) {
                showPage.bandAskedFor = true
                showPage.requestSlideBand()
            }
        }
    }
    Component.onCompleted: Qt.callLater(function() { showPage.requestSlideBand() })

    Rectangle {
        id: slideListBox
        anchors.top: slideActions.bottom
        anchors.topMargin: 8
        anchors.left: parent.left
        anchors.right: parent.right
        height: 150
        color: "#12121c"
        border.color: "#2f3542"

        ListView {
            id: slideList
            anchors.fill: parent
            anchors.margins: 1
            clip: true
            model: docs.slides

            delegate: ItemDelegate {
                id: control
                required property var modelData
                width: slideList.width
                height: 26
                text: (modelData.index + 1) + ".  " + modelData.preview
                highlighted: modelData.index === docs.currentSlide
                contentItem: Row {
                    spacing: 6
                    // The worker's own picture of this slide (task 6.16b).
                    // It is an addition, not a replacement: the text line
                    // stays, so a row is legible before its picture
                    // arrives, on a worker that cannot draw the list, and
                    // to a screen reader either way.
                    PreviewView {
                        id: rowPicture
                        visible: worker.thumbnailSupported
                        width: 46
                        height: 26
                        anchors.verticalCenter: parent.verticalCenter
                        controller: worker
                        target: PreviewView.Thumbnail
                        key: modelData.id
                    }
                    Text {
                        // The picture's 46 px only when there is one: a
                        // positioner lays out nothing for an invisible
                        // child, so narrowing the line for a picture that
                        // is not there would elide it for no reason.
                        width: slideList.width
                               - (rowPicture.visible ? rowPicture.width : 0) - 40
                        text: control.text
                        color: control.highlighted ? "#ffffff" : "#f0f0ff"
                        font.pixelSize: 13
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                    }
                }
                background: Rectangle {
                    color: control.highlighted ? "#f0008c"
                         : (control.hovered ? "#2f3542" : "transparent")
                }
                onClicked: {
                    docs.selectSlide(modelData.index)
                    if (showPage.following) {
                        showPage.presentRequested()
                    }
                }
            }
        }
    }

    Row {
        id: historyRow
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        spacing: 8

        Button {
            text: "Undo"
            enabled: docs.canUndo
            onClicked: docs.undo()
        }

        Button {
            text: "Redo"
            enabled: docs.canRedo
            onClicked: docs.redo()
        }

        Button {
            text: "Save"
            enabled: docs.dirty
            onClicked: docs.save()
        }

        Button {
            text: "Load"
            onClicked: docs.load()
        }
    }

    Row {
        id: navRow
        anchors.bottom: historyRow.top
        anchors.bottomMargin: 10
        anchors.left: parent.left
        spacing: 8

        Button {
            text: "\u25c0 Prev"
            enabled: docs.slides.length > 0
            onClicked: {
                docs.stepSlide(-1)
                if (showPage.following) {
                    showPage.presentRequested()
                }
            }
        }

        Button {
            text: "Next \u25b6"
            enabled: docs.slides.length > 0
            onClicked: {
                docs.stepSlide(1)
                if (showPage.following) {
                    showPage.presentRequested()
                }
            }
        }

        Button {
            text: "Present"
            enabled: docs.slides.length > 0 && worker.outputLive
            onClicked: {
                showPage.following = true
                showPage.presentRequested()
            }
        }

        CheckBox {
            text: "follow"
            checked: showPage.following
            onToggled: showPage.following = checked
        }
    }

    // The editor writes into the debounced controller slot; the
    // binding is re-established (not just assigned) when navigation,
    // undo or a load changes the slide under the operator, so future
    // changes keep flowing without breaking typing again. Since
    // 6.17b2 the selection may be a picture, which has no words to
    // edit — offering an empty field for one invites a typing burst
    // the model must refuse, so the dock shows the picture's asset
    // instead, and the field returns the moment a text box is
    // selected again.
    Column {
        id: pictureInfo
        objectName: "pictureInfo"
        anchors.top: slideListBox.bottom
        anchors.topMargin: 10
        anchors.bottom: navRow.top
        anchors.bottomMargin: 10
        anchors.left: parent.left
        anchors.right: parent.right
        visible: docs.currentBoxKind === "image"
        spacing: 6

        Text {
            color: "#8f93a5"
            font.pixelSize: 10
            font.bold: true
            text: "PICTURE"
        }
        Text {
            objectName: "pictureAssetName"
            width: parent.width
            elide: Text.ElideMiddle
            color: "#f0f0ff"
            font.pixelSize: 14
            text: docs.currentBoxAsset.name || "this picture has lost its asset"
        }
        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            color: "#8f93a5"
            font.pixelSize: 11
            text: docs.currentBoxAsset.sourcePath || " "
        }
        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            color: "#6a6f80"
            font.pixelSize: 11
            text: "drag on the canvas to place it \u00b7 Back/Forward reorder "
                  + "\u00b7 Delete takes it off the slide"
        }
    }

    TextArea {
        id: lyricText
        visible: docs.currentBoxKind !== "image"
        anchors.top: slideListBox.bottom
        anchors.topMargin: 10
        anchors.bottom: navRow.top
        anchors.bottomMargin: 10
        anchors.left: parent.left
        anchors.right: parent.right
        text: docs.currentSlideText
        onTextChanged: docs.setSlideText(text)
        selectByMouse: true
        wrapMode: TextArea.Wrap
        color: "#f0f0ff"
        background: Rectangle {
            color: "#12121c"
            border.color: "#2f3542"
        }

        Connections {
            target: docs
            function onSlideTextChanged() {
                lyricText.text = Qt.binding(function () { return docs.currentSlideText })
                lyricText.cursorPosition = 0
            }
        }
    }
}
