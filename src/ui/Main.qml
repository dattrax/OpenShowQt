import QtQuick
import QtQuick.Controls

Window {
    id: root
    width: 1380
    height: 800
    visible: true
    color: "#141414"
    title: "OpenShowQt " + applicationVersion

    // Task 1.2: closing the UI shuts the worker down before the window
    // actually goes away.
    onClosing: function(close) {
        worker.shutdownNow()
    }

    // Task 3.3: once a slide has been presented from the panel, navigating
    // follows it onto the output — that is the operator's expectation
    // mid-service. Any Present click (re)arms it.
    property bool following: false

    function presentCurrentSlide() {
        const json = docs.currentSceneJson()
        if (json !== "") {
            worker.presentSceneJson(json)
        }
    }

    // Task 3.4: the reference's presenter keys. Escape clears all — and
    // restores when the output is already clear, matching the button's
    // Clear-all/Restore toggle. Disabled controls (nothing on air) simply
    // do nothing.
    Shortcut {
        sequence: StandardKey.Cancel
        enabled: worker.wordsOnAir || worker.backgroundOnAir || worker.canRestore
        onActivated: worker.canRestore ? worker.restoreOutput() : worker.clearAll()
    }
    Shortcut {
        sequence: "F1"
        enabled: worker.backgroundOnAir
        onActivated: worker.clearBackground()
    }
    Shortcut {
        sequence: "F2"
        enabled: worker.wordsOnAir
        onActivated: worker.clearSlide()
    }

    Column {
        id: operatorPanel
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.margins: 16
        width: 500
        spacing: 14

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            color: "#e8e8e8"
            font.pixelSize: 22
            text: "OpenShowQt " + applicationVersion
        }

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            color: worker.outputLive ? "#7dd47d" : "#d47d7d"
            font.pixelSize: 15
            text: worker.status
        }

        Text {
            visible: worker.outputLive
            anchors.horizontalCenter: parent.horizontalCenter
            color: "#9a9a9a"
            font.pixelSize: 13
            // The panel name is looked up in displays, guarded: status can
            // arrive before the display list (or after a hotplug empties it).
            text: {
                const panel = worker.currentMonitor >= 0
                    ? worker.displays[worker.currentMonitor] : null
                const where = worker.currentMonitor < 0 ? "windowed"
                    : (panel ? panel.name : "monitor " + worker.currentMonitor)
                return worker.device + " · frame " + worker.frames
                       + " · revision " + worker.revision + " · " + where
            }
        }

        Row {
            visible: worker.outputLive
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 8

            Text {
                anchors.verticalCenter: parent.verticalCenter
                color: "#9a9a9a"
                font.pixelSize: 13
                text: "Destination:"
            }

            ComboBox {
                id: destination
                model: [{value: -1, label: "Windowed"}].concat(
                    worker.displays.map(d => ({value: d.index,
                                               label: d.name + " — " + d.size})))
                textRole: "label"
                valueRole: "value"
                // Bound to the worker's applied state, so the picker shows
                // where the output actually is until a switch confirms.
                currentIndex: destination.indexOfValue(worker.currentMonitor)
                onActivated: worker.selectDisplay(currentValue)
            }
        }

        Row {
            visible: worker.outputLive
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 10

            Repeater {
                model: ["background", "images", "words", "aux", "clock"]

                delegate: CheckBox {
                    required property string modelData
                    text: modelData
                    checked: worker.layers[modelData] === true
                    onToggled: worker.setLayerEnabled(modelData, checked)
                }
            }
        }

        // Task 2.4c: compiled scenes through the same path real content
        // will take — presentDemoSlide compiles the fixture's slide with
        // compileSlide, prepares it, and presents the revision the
        // worker acknowledges. The label shows the worker's store.
        Row {
            visible: worker.outputLive && worker.demoSlideCount > 0
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 10

            Repeater {
                model: worker.demoSlideCount

                Button {
                    required property int index
                    text: "scene " + (index + 1)
                    onClicked: worker.presentDemoSlide(index)
                }
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                color: "#9a9a9a"
                font.pixelSize: 12
                text: "scene " + worker.sceneActive
                      + (worker.scenePending !== worker.sceneActive
                         ? " \u2192 " + worker.scenePending : "")
                      + " \u00b7 " + worker.scenePrepared + " prepared"
            }
        }

        // Task 1.4: the audience output as the worker sees it, streamed
        // over shared memory. Qt only paints bytes here; the scene is the
        // worker's Skia render, downscaled to preview size.
        Column {
            visible: worker.outputLive
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 4

            Rectangle {
                width: 480
                height: 270
                color: "black"
                border.color: "#3a3a3a"

                PreviewView {
                    anchors.fill: parent
                    anchors.margins: 1
                    controller: worker
                }
            }

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                color: "#9a9a9a"
                font.pixelSize: 12
                text: worker.previewActive
                    ? "preview \u00b7 " + worker.previewFrames + " frames shown \u00b7 "
                      + worker.previewDropped + " dropped"
                    : (worker.previewError !== ""
                       ? "preview: " + worker.previewError : "preview starting...")
            }
        }
    }

    /* ------------------------------------------------------------------
     * Task 3.3: the lyric panel. Everything here drives DocumentController
     * ("docs"): songs, their slides, the current slide's text, navigation,
     * undo/redo and service save/load. Presenting compiles the selected
     * slide to a scene (compileSlide) and hands the JSON to the worker,
     * which prepares it and presents on the acknowledgement.
     * ------------------------------------------------------------------ */
    Column {
        id: lyricPanel
        anchors.left: operatorPanel.right
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.leftMargin: 16
        anchors.topMargin: 16
        anchors.bottomMargin: 16
        spacing: 8

        Row {
            spacing: 8

            TextField {
                id: songName
                width: 220
                placeholderText: "new song name"
                placeholderTextColor: "#6a6a6a"
                color: "#e8e8e8"
                onAccepted: createSongButton.clicked()
            }

            Button {
                id: createSongButton
                text: "New song"
                enabled: songName.text.trim().length > 0
                onClicked: if (docs.createSong(songName.text)) songName.text = ""
            }

            Button {
                text: "Remove song"
                enabled: docs.currentSongId !== ""
                onClicked: docs.removeCurrentSong()
            }
        }

        Rectangle {
            width: parent.width
            height: 108
            color: "#1c1c1c"
            border.color: "#3a3a3a"

            ListView {
                id: songList
                anchors.fill: parent
                anchors.margins: 1
                clip: true
                model: docs.songs

                delegate: ItemDelegate {
                    required property var modelData
                    width: songList.width
                    height: 28
                    text: modelData.name + "  \u00b7  " + modelData.slideCount + " slides"
                    highlighted: modelData.id === docs.currentSongId
                    onClicked: docs.selectSong(modelData.id)
                }

                Text {
                    anchors.centerIn: parent
                    visible: songList.count === 0
                    color: "#6a6a6a"
                    font.pixelSize: 13
                    text: "no songs \u2014 create one above"
                }
            }
        }

        Row {
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
                color: "#9a9a9a"
                font.pixelSize: 12
                text: docs.slides.length > 0
                    ? "slide " + (docs.currentSlide + 1) + " of " + docs.slides.length
                    : "no slides"
            }
        }

        Rectangle {
            width: parent.width
            height: 132
            color: "#1c1c1c"
            border.color: "#3a3a3a"

            ListView {
                id: slideList
                anchors.fill: parent
                anchors.margins: 1
                clip: true
                model: docs.slides

                delegate: ItemDelegate {
                    required property var modelData
                    width: slideList.width
                    height: 26
                    text: (modelData.index + 1) + ".  " + modelData.preview
                    highlighted: modelData.index === docs.currentSlide
                    onClicked: {
                        docs.selectSlide(modelData.index)
                        if (root.following) {
                            root.presentCurrentSlide()
                        }
                    }
                }
            }
        }

        // The editor writes into the debounced controller slot; the
        // binding is re-established (not just assigned) when navigation,
        // undo or a load changes the slide under the operator, so future
        // changes keep flowing without breaking typing again.
        TextArea {
            id: lyricText
            width: parent.width
            height: 200
            text: docs.currentSlideText
            onTextChanged: docs.setSlideText(text)
            selectByMouse: true
            wrapMode: TextArea.Wrap
            color: "#e8e8e8"
            background: Rectangle {
                color: "#101010"
                border.color: "#3a3a3a"
            }

            Connections {
                target: docs
                function onSlideTextChanged() {
                    lyricText.text = Qt.binding(function () { return docs.currentSlideText })
                    lyricText.cursorPosition = 0
                }
            }
        }

        Row {
            spacing: 8

            Button {
                text: "\u25c0 Prev"
                enabled: docs.slides.length > 0
                onClicked: {
                    docs.stepSlide(-1)
                    if (root.following) {
                        root.presentCurrentSlide()
                    }
                }
            }

            Button {
                text: "Next \u25b6"
                enabled: docs.slides.length > 0
                onClicked: {
                    docs.stepSlide(1)
                    if (root.following) {
                        root.presentCurrentSlide()
                    }
                }
            }

            Button {
                text: "Present"
                enabled: docs.slides.length > 0 && worker.outputLive
                onClicked: {
                    root.following = true
                    root.presentCurrentSlide()
                }
            }

            CheckBox {
                text: "follow"
                checked: root.following
                onToggled: root.following = checked
            }
        }

        // Task 3.4: output controls. Enabled state and live flags come
        // from the worker's acknowledged active scene, never from a click:
        // each clear is disabled while its own layer is off air (the
        // reference's "independent per-layer clearing made visible"), and
        // Clear all becomes Restore output once the output is clear.
        Row {
            spacing: 8

            Button {
                text: "Clear slide"
                enabled: worker.wordsOnAir
                onClicked: worker.clearSlide()
            }

            Button {
                text: "Clear background"
                enabled: worker.backgroundOnAir
                onClicked: worker.clearBackground()
            }

            Button {
                text: worker.canRestore ? "Restore output" : "Clear all"
                enabled: worker.wordsOnAir || worker.backgroundOnAir || worker.canRestore
                onClicked: worker.canRestore ? worker.restoreOutput() : worker.clearAll()
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                color: "#9a9a9a"
                font.pixelSize: 12
                text: {
                    const onAir = []
                    if (worker.backgroundOnAir) onAir.push("background")
                    if (worker.wordsOnAir) onAir.push("words")
                    return onAir.length > 0 ? "on air: " + onAir.join(" + ") : "output clear"
                }
            }
        }

        Row {
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

        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            color: docs.dirty ? "#d8c07a" : "#9a9a9a"
            font.pixelSize: 12
            text: (docs.dirty ? "\u25cf unsaved  \u00b7  " : "")
                  + (docs.status !== "" ? docs.status + "  \u00b7  " : "")
                  + docs.serviceDirectory
        }
    }
}
