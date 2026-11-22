import QtQuick
import QtQuick.Controls

// UI.5: the right rail is the show's runtime monitor, arranged the way the
// plan's diagram draws it — the audience picture with compact clear controls
// BESIDE it, and the media transport beneath. Nothing here configures the
// output: no routing, no layers, no renderer settings (those live in
// Settings), and no control speaks ahead of the worker. Every enabled state,
// every marker and every number on this rail mirrors an acknowledged status
// report; the rail itself decides nothing, sends nothing except through the
// controller's confirmed commands, and never lets a browse or a selection
// imply air.
//
// The clear controls are the reference's pair shape: a small red action over
// its label, one pair per layer the product can clear, each pair disabled
// while its own layer is empty — independent per-layer clearing made
// visible. "Clear all" is a content operation, so it stays that word; when
// a restore exists it becomes "Restore", which is exactly what the worker
// still holds.
//
// Audio is a cue like every other kind, so it has no separate area or Play
// button here: the transport governs whatever is on air, video or audio
// alike. The one audio-layer control is the audience mute at the TOP of the
// rail — one switch that silences the background's embedded audio and the
// show's own audio layer together through media.mute source "all". It sits
// with the picture and the layer clears rather than inside a source's row,
// so it is never "this source's mute": whatever is on air is silenced, and
// releasing it restores the mix underneath. The per-source rows below the
// transport keep only the worker's gain echo, each labelled as a volume, so
// the operator shapes each layer under the master without a second mute.
SidePanel {
    id: outputPanel
    side: "right"
    function fileName(path) { return String(path).split(/[\\/]/).pop() || "Unknown file" }
    // Review D5: output activity includes the cue's own audio layer. A cue
    // that is only audible (no words, no picture) is still output, so the
    // monitor must not call it clear and Clear All must stay reachable.
    readonly property bool outputActive: worker.wordsOnAir || worker.backgroundOnAir
                                         || worker.audioOnAir
    readonly property string destinationName: worker.currentMonitor < 0 ? "Windowed"
        : (worker.displays.filter(d => d.index === worker.currentMonitor).map(d => d.name).join("") || "Audience display")

    // The background transport's wire position is LOOP-CONTINUOUS: a looping
    // clip's clock keeps climbing past its length instead of restarting at 0,
    // because that continuity is what its pacing and absolute seeks need (the
    // worker's status carries exactly that clock, and its tests assert it).
    // The rail shows the clip the audience is ACTUALLY in, so the position
    // wraps back into the clip. Without the wrap the slider's range is
    // 0..duration while the reported position runs past it: after the first
    // repeat the slider pins at the right edge and never returns to the
    // start, even though the picture is playing the clip again. A finished
    // clip with loop off keeps its end — that really is where it stopped —
    // rather than claiming a restart nobody asked for.
    function transportPositionOf(position, duration, looping) {
        if (position < 0 || duration <= 0 || position < duration)
            return position
        return looping ? position % duration : duration
    }
    readonly property real transportPosition:
        transportPositionOf(worker.mediaPosition, worker.mediaDuration, worker.mediaLoop)
    // People read a running time as minutes and seconds, not tenths of
    // seconds: once either end reaches a minute the readout switches to
    // m:ss for BOTH numbers (0:07 / 4:28), and keeps one decimal only in the
    // short clips where tenths are still useful.
    readonly property bool clockFormat:
        transportPosition >= 60 || worker.mediaDuration >= 60
    function clockText(seconds, useClock) {
        if (seconds < 0) return "\u2014"
        if (!useClock) return seconds.toFixed(1) + " s"
        const total = Math.round(seconds)
        const minutes = Math.floor(total / 60)
        const rest = total % 60
        return minutes + ":" + (rest < 10 ? "0" : "") + rest
    }

    // The reference's per-layer clear pair (ClearButtons.svelte): a 24px red
    // action over its label; the pair is dead while its own layer is empty,
    // the label lights while that layer is on air.
    component LayerClear: Column {
        id: layer
        property string label: ""
        property bool layerOn: false
        signal cleared()
        // The pair is dead while its own layer is empty — on the pair, not
        // just its button, so the whole control says what it means.
        enabled: layer.layerOn
        spacing: 2
        Button {
            id: cross
            width: parent.width
            height: 24
            text: "\u2715"
            font.pixelSize: 12
            focusPolicy: Qt.NoFocus
            enabled: layer.layerOn
            onClicked: layer.cleared()
            background: Rectangle {
                radius: 3
                color: cross.down ? "#8f3737" : "#5c2626"
                border.color: "#7c3a3a"
                opacity: cross.enabled ? 1.0 : 0.45
            }
            contentItem: Text {
                text: cross.text
                font: cross.font
                color: cross.enabled ? "#ffd9d9" : "#8f8f8f"
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
        }
        Text {
            width: parent.width
            text: layer.label
            font.pixelSize: 10
            color: layer.layerOn ? "#7cc0f0" : "#9a9a9a"
            elide: Text.ElideRight
        }
    }

    // The label a mix row wears: the source's audience-facing name plus the
    // one word that says what its slider does. A function, so the workspace
    // test reads the same sentence the operator does and a source the
    // transport reports without a friendly name still names itself.
    function sourceName(id) {
        return id === "video" ? "Video audio"
             : (id === "audio" ? "Show audio" : id)
    }
    function sourceVolumeLabel(id) { return sourceName(id) + " volume" }

    // One source's volume, as the worker reports it: the gain echo is sent
    // back as media.gain and then confirmed by the next status block — a
    // refused change snaps the slider back. Mute lives once at the top of the
    // rail, so these rows are volumes and nothing else: the label says so and
    // the percentage reads the acknowledged value.
    component SourceMix: Column {
        id: sourceMix
        required property var source
        property string label: ""
        spacing: 2
        Item {
            width: parent.width
            height: volumeLabel.implicitHeight
            Text {
                id: volumeLabel
                text: sourceMix.label
                color: "#b0b0b0"; font.pixelSize: 11
            }
            Text {
                anchors.right: parent.right
                anchors.baseline: volumeLabel.baseline
                text: Math.round(gainSlider.value * 100) + "%"
                color: "#8f8f8f"; font.pixelSize: 11
            }
        }
        Slider {
            id: gainSlider
            objectName: "sourceGainSlider"
            Accessible.name: sourceMix.label
            width: parent.width
            from: 0; to: 1
            value: sourceMix.source ? sourceMix.source.gain : 0
            // The handler runs before the binding re-evaluates, so the
            // guard is what keeps a status tick from reaching a source
            // that has just gone away (a scene swap while dragged).
            onMoved: if (sourceMix.source) worker.setMediaGain(sourceMix.source.id, value)
            Connections {
                target: worker
                function onMediaChanged() {
                    if (!gainSlider.pressed && sourceMix.source)
                        gainSlider.value = sourceMix.source.gain
                }
            }
        }
    }

    width: panels.rightWidth
    onDraggedTo: candidate => panels.resizeRight(candidate)
    onStripClicked: panels.clickRight()

    Item {
        anchors.fill: parent
        anchors.margins: 10
        // The audience header: the rail's name on the left and its one MUTE
        // control on the right. The mute belongs here, above the monitor and
        // the layer clears, because it is an operation on the audience's
        // whole sound rather than on a named source: one press silences the
        // background's embedded audio and the show track together, and the
        // next press gives back the mix the volume rows below describe.
        Item {
            id: title
            objectName: "audienceHeader"
            width: parent.width
            height: 26
            Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                color: "#bbbbbb"; font.pixelSize: 11; font.bold: true
                text: "AUDIENCE"
            }
            PanelButton {
                objectName: "masterMuteButton"
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                height: 24
                // The label is the action when audio is audible and the state
                // when it is not, so a glance reads the output, not a tick.
                text: worker.mediaMuted ? "Audio muted" : "Mute audio"
                highlighted: worker.mediaMuted
                enabled: worker.connected
                Accessible.name: worker.mediaMuted
                                 ? "Unmute audience audio" : "Mute audience audio"
                onClicked: worker.setMediaMasterMute(!worker.mediaMuted)
            }
        }

        // The monitor and its clears: the picture nearest the screen edge,
        // the compact clear strip beside it, as the plan's layout draws
        // them. The strip never covers the picture, and the picture is the
        // audience's own stream (PreviewView reads the worker's copied
        // frames) — the edit surface's selection preview is a different
        // pane fed by a different target, and no selection here paints it.
        Row {
            id: monitorRow
            objectName: "audienceMonitorRow"
            anchors.top: title.bottom
            width: parent.width
            spacing: 8
            // The Audience monitor (task CS.2): one SlidePreviewSurface, the
            // only wearer of the Audience target. The clears beside it are
            // operations ON the output and stay outside the surface.
            SlidePreviewSurface {
                id: previewBox
                objectName: "audiencePreview"
                width: monitorRow.width - clears.width - monitorRow.spacing
                height: width * 9 / 16
                controller: worker
                target: "audience"
                borderColor: "#505050"
                borderWidth: 0
                placeholder: "Output disconnected"
                placeholderVisible: !worker.outputLive
                placeholderColor: "#aaaaaa"
                placeholderPixelSize: 12
                placeholderObjectName: "previewOfflineNote"
            }
            Column {
                id: clears
                objectName: "clearControls"
                width: 88
                spacing: 6
                // Clears are operations ON the output: with no live output
                // there is nothing to clear and no snapshot to restore, and
                // the buttons say so rather than eating a click.
                enabled: worker.outputLive
                LayerClear {
                    objectName: "clearSlideControl"
                    Accessible.name: "Clear slide"
                    width: parent.width
                    label: "Slide"
                    layerOn: worker.wordsOnAir
                    onCleared: worker.clearSlide()
                }
                LayerClear {
                    objectName: "clearBackgroundControl"
                    Accessible.name: "Clear background"
                    width: parent.width
                    label: "Background"
                    layerOn: worker.backgroundOnAir
                    onCleared: worker.clearBackground()
                }
                PanelButton {
                    objectName: "clearAllButton"
                    width: parent.width
                    font.pixelSize: 11
                    text: worker.canRestore ? "Restore" : "Clear all"
                    Accessible.name: worker.canRestore ? "Restore output" : "Clear all"
                    enabled: outputPanel.outputActive || worker.canRestore
                    onClicked: worker.canRestore ? worker.restoreOutput() : worker.clearAll()
                }
            }
        }

        Text {
            id: state
            anchors.top: monitorRow.bottom; anchors.topMargin: 8
            width: parent.width
            color: worker.outputLive ? "#b8d7b0" : "#d89b9b"
            font.pixelSize: 11
            text: !worker.outputLive ? "Audience output unavailable"
                : (outputPanel.outputActive ? "\u25cf  Output active" : "\u25cb  Output clear")
                  + " \u00b7 " + outputPanel.destinationName
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

                // Milestone FO: the layer matrix lives under its own dialog
                // (rows are layers, columns are destinations). Opening it
                // changes no output; Apply stores the desired masks.
                PanelButton {
                    objectName: "outputLayersButton"
                    width: parent.width
                    text: "Output layers…"
                    Accessible.name: "Configure output layers"
                    onClicked: outputLayersDialog.open()
                }

                // ---------------- transport ----------------
                // The on-air slide's video, played, seeked and looped as the
                // worker confirms it. A video the presenter wants heard is
                // authored into a slide in Edit — this transport governs the
                // one that is on air, and with nothing on air it says so.
                Text {
                    objectName: "transportHeading"
                    text: "TRANSPORT"; color: "#bbbbbb"; font.pixelSize: 11; font.bold: true
                }
                Text {
                    width: parent.width
                    visible: worker.foregroundErrors.length > 0
                    wrapMode: Text.WordWrap; color: "#d9a441"; font.pixelSize: 11
                    text: worker.foregroundErrors.map(f => "Slide image unavailable: " + outputPanel.fileName(f.path)).join("\n")
                }
                Text {
                    visible: worker.mediaError !== "" || worker.mediaAudio === "degraded" || worker.mediaAudio === "no-output"
                    wrapMode: Text.WordWrap; color: "#d9a441"; font.pixelSize: 11
                    text: worker.mediaError !== "" ? "Cannot play media: " + outputPanel.fileName(worker.mediaPath)
                          : "Media audio is unavailable"
                }
                Text {
                    objectName: "transportIdleNote"
                    visible: !worker.mediaOnAir
                    text: "nothing on the transport"; color: "#929292"; font.pixelSize: 12
                }
                Column {
                    width: parent.width; spacing: 6
                    visible: worker.mediaOnAir
                    Row {
                        spacing: 8
                        PanelButton {
                            objectName: "transportPlayButton"
                            text: worker.mediaPlaying ? "Pause" : "Play"
                            Accessible.name: worker.mediaPlaying ? "Pause video" : "Play video"
                            onClicked: worker.mediaPlaying ? worker.mediaPause() : worker.mediaPlay()
                        }
                        CheckBox {
                            objectName: "transportLoopCheck"
                            text: "Loop"; font.pixelSize: 12
                            checked: worker.mediaLoop
                            onToggled: worker.setMediaLoop(checked)
                        }
                    }
                    Text {
                        objectName: "transportTimeText"
                        color: "#b0b0b0"; font.pixelSize: 12
                        text: outputPanel.transportPosition < 0 ? "\u2014"
                            : outputPanel.clockText(outputPanel.transportPosition,
                                                    outputPanel.clockFormat)
                              + (worker.mediaDuration > 0
                                 ? " / " + outputPanel.clockText(worker.mediaDuration,
                                                                 outputPanel.clockFormat) : "")
                    }
                    Slider {
                        id: seekSlider
                        objectName: "transportSeekSlider"
                        width: parent.width
                        visible: worker.mediaDuration > 0
                        from: 0; to: Math.max(worker.mediaDuration, 0.0001)
                        onMoved: worker.mediaSeek(value)
                        Connections {
                            target: worker
                            function onMediaChanged() {
                                if (!seekSlider.pressed) seekSlider.value = outputPanel.transportPosition
                            }
                        }
                    }
                }
                Repeater {
                    model: worker.mediaSources.filter(s => s.id !== "standalone")
                    delegate: SourceMix {
                        required property var modelData
                        source: modelData
                        label: outputPanel.sourceVolumeLabel(modelData.id)
                        width: parent.width
                    }
                }

            }
        }
    }

    OutputLayersDialog {
        id: outputLayersDialog
    }
}
