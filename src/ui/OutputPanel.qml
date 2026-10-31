import QtQuick
import QtQuick.Controls

// The audience picture stays in place; only the controls below it scroll.
SidePanel {
    id: outputPanel
    side: "right"
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
            text: !worker.outputLive ? worker.status
                : worker.wordsOnAir || worker.backgroundOnAir ? "\u25cf  Output active" : "\u25cb  Output clear"
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
                    text: worker.foregroundErrors.map(f => "Picture not drawn: " + f.path + " — " + f.error).join("\n")
                }
                Rectangle { width: parent.width; height: 1; color: "#505050" }
                Text { text: "MEDIA"; color: "#bbbbbb"; font.pixelSize: 11; font.bold: true }
                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap; color: "#d9a441"; font.pixelSize: 11
                    visible: worker.mediaError !== "" || worker.mediaAudio === "degraded" || worker.mediaAudio === "no-output"
                    text: worker.mediaError || ("Audio: " + worker.mediaAudio)
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
                        Text { text: sourceRow.modelData.id; color: "#b0b0b0"; font.pixelSize: 11 }
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
                PanelField {
                    id: standaloneField
                    width: parent.width
                    placeholderText: "Audio file path"
                }
                Row {
                    spacing: 6
                    PanelButton {
                        text: "Set track"
                        enabled: standaloneField.text.trim().length > 0
                        onClicked: worker.selectStandaloneTrack(standaloneField.text.trim())
                    }
                    PanelButton {
                        text: "Remove track"; enabled: worker.standaloneOnAir
                        onClicked: worker.selectStandaloneTrack("")
                    }
                }
                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap; font.pixelSize: 11
                    color: worker.standaloneError.length > 0 ? "#d9a441" : "#929292"
                    text: worker.standaloneError || (worker.standaloneOnAir
                        ? worker.standalonePath + (worker.standalonePosition >= 0 ? " · " + worker.standalonePosition.toFixed(1) + " s" : "")
                        : "No audio track selected")
                }
                Rectangle { width: parent.width; height: 1; color: "#505050" }
                Text { text: "OUTPUT"; color: "#bbbbbb"; font.pixelSize: 11; font.bold: true }
                ComboBox {
                    id: destination
                    width: parent.width
                    model: [{value: -1, label: "Windowed"}].concat(
                        worker.displays.map(d => ({value: d.index, label: d.name + " — " + d.size})))
                    textRole: "label"; valueRole: "value"
                    currentIndex: destination.indexOfValue(worker.currentMonitor)
                    onActivated: worker.selectDisplay(currentValue)
                }
                Flow {
                    width: parent.width; spacing: 4
                    Repeater {
                        model: ["background", "images", "words", "aux", "clock"]
                        delegate: CheckBox {
                            required property string modelData
                            text: modelData; font.pixelSize: 11
                            checked: worker.layers[modelData] === true
                            onToggled: worker.setLayerEnabled(modelData, checked)
                        }
                    }
                }
            }
        }
    }
}
