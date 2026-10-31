import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs

// The audience picture stays in place; only the controls below it scroll.
SidePanel {
    id: outputPanel
    side: "right"
    function fileName(path) { return String(path).split(/[\\/]/).pop() || "Unknown file" }
    readonly property string destinationName: worker.currentMonitor < 0 ? "Windowed"
        : (worker.displays.filter(d => d.index === worker.currentMonitor).map(d => d.name).join("") || "Audience display")
    FileDialog {
        id: audioPicker
        title: "Play audio"
        modality: Qt.ApplicationModal
        acceptLabel: "Play"
        nameFilters: ["Audio files (*.mp3 *.wav *.ogg *.m4a *.aac *.flac *.opus)", "All files (*)"]
        onAccepted: worker.selectStandaloneFile(selectedFile)
    }
    width: panels.rightWidth
    onDraggedTo: candidate => panels.resizeRight(candidate)
    onStripClicked: panels.clickRight()

    Item {
        anchors.fill: parent
        anchors.margins: 10
        Text {
            id: title
            height: 24
            color: "#bbbbbb"; font.pixelSize: 11; font.bold: true
            text: "AUDIENCE"
        }
        Rectangle {
            id: previewBox
            objectName: "audiencePreview"
            anchors.top: title.bottom
            width: parent.width; height: width * 9 / 16
            color: "black"
            border.color: "#505050"
            PreviewView { anchors.fill: parent; anchors.margins: 1; controller: worker }
            Text {
                anchors.centerIn: parent
                visible: !worker.outputLive
                color: "#aaaaaa"; font.pixelSize: 12
                text: "Output disconnected"
            }
        }
        Text {
            id: state
            anchors.top: previewBox.bottom; anchors.topMargin: 8
            width: parent.width
            color: worker.outputLive ? "#b8d7b0" : "#d89b9b"
            font.pixelSize: 11
            text: !worker.outputLive ? "Audience output unavailable"
                : (worker.wordsOnAir || worker.backgroundOnAir ? "\u25cf  Output active" : "\u25cb  Output clear")
                  + " · " + outputPanel.destinationName
            elide: Text.ElideRight
        }
        ScrollView {
            id: toolsScroll
            objectName: "outputControlsScroll"
            anchors.top: state.bottom; anchors.topMargin: 12
            anchors.bottom: parent.bottom
            width: parent.width
            contentWidth: availableWidth
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            Column {
                width: toolsScroll.availableWidth
                spacing: 12
                enabled: worker.outputLive
                Flow {
                    width: parent.width; spacing: 6
                    PanelButton { text: "Clear slide"; enabled: worker.wordsOnAir; onClicked: worker.clearSlide() }
                    PanelButton { text: "Clear background"; enabled: worker.backgroundOnAir; onClicked: worker.clearBackground() }
                    PanelButton {
                        text: worker.canRestore ? "Restore output" : "Clear all"
                        enabled: worker.wordsOnAir || worker.backgroundOnAir || worker.canRestore
                        onClicked: worker.canRestore ? worker.restoreOutput() : worker.clearAll()
                    }
                }
                Text {
                    width: parent.width
                    visible: worker.foregroundErrors.length > 0
                    wrapMode: Text.WordWrap; color: "#d9a441"; font.pixelSize: 11
                    text: worker.foregroundErrors.map(f => "Slide image unavailable: " + outputPanel.fileName(f.path)).join("\n")
                }
                Rectangle { width: parent.width; height: 1; color: "#505050" }
                Text { text: "MEDIA"; color: "#bbbbbb"; font.pixelSize: 11; font.bold: true }
                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap; color: "#d9a441"; font.pixelSize: 11
                    visible: worker.mediaError !== "" || worker.mediaAudio === "degraded" || worker.mediaAudio === "no-output"
                    text: worker.mediaError !== "" ? "Cannot play media: " + outputPanel.fileName(worker.mediaPath)
                          : "Media audio is unavailable"
                }
                Column {
                    width: parent.width; spacing: 6
                    visible: worker.mediaOnAir
                    Row {
                        spacing: 8
                        PanelButton {
                            text: worker.mediaPlaying ? "Pause" : "Play"
                            onClicked: worker.mediaPlaying ? worker.mediaPause() : worker.mediaPlay()
                        }
                        CheckBox {
                            text: "Loop"; font.pixelSize: 12
                            checked: worker.mediaLoop
                            onToggled: worker.setMediaLoop(checked)
                        }
                    }
                    Text {
                        color: "#b0b0b0"; font.pixelSize: 12
                        text: worker.mediaPosition >= 0 ? worker.mediaPosition.toFixed(1) + " s"
                            + (worker.mediaDuration > 0 ? " / " + worker.mediaDuration.toFixed(1) + " s" : "") : "—"
                    }
                    Slider {
                        id: seekSlider
                        width: parent.width
                        visible: worker.mediaDuration > 0
                        from: 0; to: Math.max(worker.mediaDuration, 0.0001)
                        onMoved: worker.mediaSeek(value)
                        Connections {
                            target: worker
                            function onMediaChanged() {
                                if (!seekSlider.pressed) seekSlider.value = worker.mediaPosition
                            }
                        }
                    }
                }
                Text {
                    visible: !worker.mediaOnAir && !worker.standaloneOnAir
                    text: "No media playing"; color: "#929292"; font.pixelSize: 12
                }
                Repeater {
                    model: worker.mediaSources
                    delegate: Column {
                        id: sourceRow
                        required property var modelData
                        width: parent.width
                        Text { text: sourceRow.modelData.id === "video" ? "Video audio" : "Audio track"; color: "#b0b0b0"; font.pixelSize: 11 }
                        Row {
                            width: parent.width; spacing: 6
                            Slider {
                                id: gainSlider
                                width: Math.max(60, sourceRow.width - mute.width - 6)
                                from: 0; to: 1; value: sourceRow.modelData.gain
                                onMoved: worker.setMediaGain(sourceRow.modelData.id, value)
                                Connections {
                                    target: worker
                                    function onMediaChanged() {
                                        if (!gainSlider.pressed) gainSlider.value = sourceRow.modelData.gain
                                    }
                                }
                            }
                            CheckBox {
                                id: mute
                                text: "Mute"; font.pixelSize: 12
                                checked: sourceRow.modelData.muted
                                onToggled: worker.setMediaMute(sourceRow.modelData.id, checked)
                            }
                        }
                    }
                }
                Text { text: "AUDIO TRACK"; color: "#bbbbbb"; font.pixelSize: 11; font.bold: true }
                Row {
                    spacing: 6
                    PanelButton {
                        objectName: "playAudioButton"
                        text: "Play audio…"
                        onClicked: audioPicker.open()
                    }
                    PanelButton {
                        objectName: "stopAudioButton"
                        text: "Stop audio"; enabled: worker.standaloneOnAir
                        onClicked: worker.selectStandaloneTrack("")
                    }
                }
                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap; font.pixelSize: 11
                    color: worker.standaloneError.length > 0 ? "#d9a441" : "#929292"
                    text: worker.standaloneError !== "" ? "Cannot play audio: " + outputPanel.fileName(worker.standalonePath)
                        : (worker.standaloneOnAir ? outputPanel.fileName(worker.standalonePath) + (worker.standalonePosition >= 0 ? " · " + worker.standalonePosition.toFixed(1) + " s" : "")
                        : "No audio track selected")
                }

            }
        }
    }
}
