import QtQuick
import QtQuick.Controls

/*
 * The right dock (task 6.19b4, from Main.qml's 1.4/2.4c/3.4/6.15a/6.15b
 * panels): the reference's Preview + ShowTools arrangement — the audience
 * output streamed over shared memory on top, the output tools below.
 *
 * Like LibraryDrawer, it is the window's side but not the window's
 * business: everything it shows mirrors `worker` status (the acknowledged
 * output model behind the scene properties, the layer toggles, the media
 * transport and the standalone track), and everything it drags reports to
 * `panels` (the reference's snap rules). What the window still owns is
 * where the dock sits — the caller anchors it between the top bar and the
 * drawer — and the SidePanel it grows out of. There are no signals here
 * because there is no window state to ask for: unlike the shortcut map,
 * this dock's commands were always the worker's, and the live-flag gates
 * (each clear disabled while its own layer is off air) read the worker's
 * answer, never a click.
 */
SidePanel {
    id: outputPanel
    side: "right"
    width: panels.rightWidth
    onDraggedTo: candidate => panels.resizeRight(candidate)
    onStripClicked: panels.clickRight()

    Item {
        anchors.fill: parent
        anchors.margins: 10

        // Task 1.4: the audience output as the worker sees it, streamed
        // over shared memory. Qt only paints bytes here; the scene is
        // the worker's Skia render, downscaled to preview size.
        Rectangle {
            id: previewBox
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: width * 9 / 16
            visible: worker.outputLive
            color: "black"
            border.color: "#2f3542"

            PreviewView {
                anchors.fill: parent
                anchors.margins: 1
                controller: worker
            }
        }

        Text {
            id: previewStats
            anchors.top: previewBox.bottom
            anchors.topMargin: 4
            anchors.left: parent.left
            visible: worker.outputLive
            color: "#8f93a5"
            font.pixelSize: 11
            text: worker.previewActive
                ? "preview \u00b7 " + worker.previewFrames + " frames shown \u00b7 "
                  + worker.previewDropped + " dropped"
                : (worker.previewError !== ""
                   ? "preview: " + worker.previewError : "preview starting...")
        }

        Column {
            id: showTools
            anchors.top: previewStats.bottom
            anchors.topMargin: 12
            anchors.left: parent.left
            anchors.right: parent.right
            spacing: 10
            visible: worker.outputLive

            Row {
                spacing: 8

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    color: "#8f93a5"
                    font.pixelSize: 12
                    text: "Output:"
                }

                ComboBox {
                    id: destination
                    width: showTools.width - 70
                    model: [{value: -1, label: "Windowed"}].concat(
                        worker.displays.map(d => ({value: d.index,
                                                   label: d.name + " — " + d.size})))
                    textRole: "label"
                    valueRole: "value"
                    // Bound to the worker's applied state, so the picker
                    // shows where the output actually is until a switch
                    // confirms.
                    currentIndex: destination.indexOfValue(worker.currentMonitor)
                    onActivated: worker.selectDisplay(currentValue)
                }
            }

            Flow {
                width: parent.width
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

            // Task 2.4c: compiled demo scenes through the same path real
            // content takes. The label shows the worker's store.
            Flow {
                width: parent.width
                spacing: 8
                visible: worker.demoSlideCount > 0

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
                    color: "#8f93a5"
                    font.pixelSize: 11
                    text: "scene " + worker.sceneActive
                          + (worker.scenePending !== worker.sceneActive
                             ? " \u2192 " + worker.scenePending : "")
                          + " \u00b7 " + worker.scenePrepared + " prepared"
                }
            }

            Rectangle {
                width: parent.width
                height: 1
                color: "#2f3542"
            }

            // Task 3.4: output controls. Enabled state and live flags
            // come from the worker's acknowledged active scene, never
            // from a click: each clear is disabled while its own layer
            // is off air (the reference's "independent per-layer
            // clearing made visible"), and Clear all becomes Restore
            // output once the output is clear.
            Flow {
                width: parent.width
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
            }

            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                color: "#8f93a5"
                font.pixelSize: 11
                text: {
                    const onAir = []
                    if (worker.backgroundOnAir) onAir.push("background")
                    if (worker.wordsOnAir) onAir.push("words")
                    return onAir.length > 0 ? "on air: " + onAir.join(" + ") : "output clear"
                }
            }

            // Task 6.15a (review R15): the media transport, live over
            // the socket. Every control mirrors worker status — the
            // Pause/Play label follows mediaPlaying, the sliders
            // follow what the worker last reported — so a refused
            // seek snaps the thumb back to where the media actually
            // is, and a loop toggle shows the intent the end of the
            // clip will honour, not the moment it was clicked.
            // Task 6.15b: the transport row belongs to the background
            // video; the mixer rows below list whatever sources are
            // registered across BOTH transports, so the column shows
            // whenever either one is on air.
            Column {
                width: parent.width
                spacing: 6
                visible: worker.mediaOnAir || worker.standaloneOnAir

                Flow {
                    width: parent.width
                    spacing: 8
                    visible: worker.mediaOnAir

                    Button {
                        text: worker.mediaPlaying ? "Pause" : "Play"
                        onClicked: worker.mediaPlaying ? worker.mediaPause()
                                                       : worker.mediaPlay()
                    }

                    CheckBox {
                        text: "Loop"
                        checked: worker.mediaLoop
                        onToggled: worker.setMediaLoop(checked)
                    }

                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        color: "#8f93a5"
                        font.pixelSize: 11
                        text: worker.mediaPosition >= 0
                              ? worker.mediaPosition.toFixed(1) + " s"
                                    + (worker.mediaDuration > 0
                                       ? " / " + worker.mediaDuration.toFixed(1) + " s"
                                       : "")
                              : "\u2014"
                    }
                }

                // Scrub only where there is a scale to scrub on: a
                // container that will not say its length gets the
                // readout above, not a slider with no right end. The
                // value is re-applied from status only while the
                // thumb is at rest, so a 500 ms poll cannot yank a
                // drag out from under the finger; every move rides the
                // transport's own newest-wins coalescing.
                Slider {
                    id: seekSlider
                    width: parent.width
                    visible: worker.mediaDuration > 0
                    from: 0
                    to: Math.max(worker.mediaDuration, 0.0001)
                    onMoved: if (active) worker.mediaSeek(value)
                    Connections {
                        target: worker
                        function onMediaChanged() {
                            if (!seekSlider.pressed && !seekSlider.dragging) {
                                seekSlider.value = worker.mediaPosition
                            }
                        }
                    }
                }

                Repeater {
                    model: worker.mediaSources

                    delegate: Row {
                        id: sourceRow
                        required property var modelData
                        spacing: 6

                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            color: "#8f93a5"
                            font.pixelSize: 11
                            text: modelData.id
                        }

                        Slider {
                            id: gainSlider
                            width: 120
                            from: 0
                            to: 1
                            value: modelData.gain
                            onMoved: if (active) worker.setMediaGain(modelData.id, value)
                            Connections {
                                target: worker
                                function onMediaChanged() {
                                    if (!gainSlider.pressed && !gainSlider.dragging) {
                                        gainSlider.value = sourceRow.modelData.gain
                                    }
                                }
                            }
                        }

                        CheckBox {
                            text: "mute"
                            checked: modelData.muted
                            onToggled: worker.setMediaMute(modelData.id, checked)
                        }
                    }
                }
            }

            // Task 6.15b (review R15): the standalone production track.
            // It keeps its OWN transport, so lyric changes, background-
            // video replacements and even the background going away
            // never restart it — which is also why its controls sit
            // outside the transport column above: they address a
            // different lifetime. The status line below is the worker's
            // report, not a hopeful selection; a refused path surfaces
            // on worker.status like any other refused command.
            Row {
                width: parent.width
                spacing: 6

                PanelField {
                    id: standaloneField
                    width: 200
                    placeholderText: "standalone track path"
                }

                PanelButton {
                    text: "Set track"
                    enabled: standaloneField.text.trim().length > 0
                             && worker.outputLive
                    onClicked: worker.selectStandaloneTrack(standaloneField.text.trim())
                }

                PanelButton {
                    text: "Remove track"
                    enabled: worker.standaloneOnAir
                    onClicked: worker.selectStandaloneTrack("")
                }
            }

            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                font.pixelSize: 11
                color: worker.standaloneError.length > 0 ? "#d9a441" : "#8f93a5"
                text: {
                    if (worker.standaloneError.length > 0) {
                        return worker.standaloneError
                    }
                    if (!worker.standaloneOnAir) {
                        return "no standalone track"
                    }
                    return "standalone \u00b7 " + worker.standalonePath
                           + (worker.standalonePosition >= 0
                              ? " \u00b7 " + worker.standalonePosition.toFixed(1) + " s"
                              : "")
                }
            }
        }

        Text {
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            visible: worker.outputLive
            wrapMode: Text.WordWrap
            color: "#8f93a5"
            font.pixelSize: 11
            // The panel name is looked up in displays, guarded: status
            // can arrive before the display list (or after a hotplug
            // empties it).
            text: {
                const panel = worker.currentMonitor >= 0
                    ? worker.displays[worker.currentMonitor] : null
                const where = worker.currentMonitor < 0 ? "windowed"
                    : (panel ? panel.name : "monitor " + worker.currentMonitor)
                return worker.device + " \u00b7 frame " + worker.frames
                       + " \u00b7 revision " + worker.revision + " \u00b7 " + where
            }
        }
    }
}
