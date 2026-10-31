import QtQuick
import QtQuick.Controls

// UI.5: the right rail is the show's runtime monitor, arranged the way the
// plan's diagram draws it — the audience picture with compact clear controls
// BESIDE it, the media transport beneath, and the production audio area
// below that. Nothing here configures the output: no routing, no layers, no
// renderer settings (those live in Settings), and no control speaks ahead of
// the worker. Every enabled state, every marker and every number on this
// rail mirrors an acknowledged status report; the rail itself decides
// nothing, sends nothing except through the controller's confirmed commands,
// and never lets a browse or a selection imply air.
//
// The clear controls are the reference's pair shape: a small red action over
// its label, one pair per layer the product can clear, each pair disabled
// while its own layer is empty — independent per-layer clearing made
// visible. "Clear all" is a content operation, so it stays that word; when
// a restore exists it becomes "Restore", which is exactly what the worker
// still holds.
//
// The audio area replaces the interim file chooser. What can be cued from
// here is the service's PREPARED audio — the bin's rows whose kind the
// domain's one name rule calls audio, not a dialog's guess from a list of
// extensions. Picking one from the list sends nothing (selection is
// browsing); Play activates it as the worker's independent production track
// — the same command and same acknowledged block as the bin's cue, so the
// two can never disagree about what is playing — and Stop is the removal.
// Whether a file will play remains the worker's probe's answer: this rail
// shows the file's NAME (Show names files; the worker's full sentence lives
// in the bin's Edit details) and it never marks air before the status block
// does.
SidePanel {
    id: outputPanel
    side: "right"
    function fileName(path) { return String(path).split(/[\\/]/).pop() || "Unknown file" }
    readonly property string destinationName: worker.currentMonitor < 0 ? "Windowed"
        : (worker.displays.filter(d => d.index === worker.currentMonitor).map(d => d.name).join("") || "Audience display")

    // The cues this area can fire: prepared assets, by the kind the domain
    // owns. A name nothing presents and a video with no runtime of its own
    // are not offered — their story is told in the bin (UI.4).
    readonly property var audioCues: docs.assets.filter(a => a.kind === "audio")
    readonly property var selectedCue: {
        const i = audioCueCombo.currentIndex
        return i >= 0 && i < outputPanel.audioCues.length ? outputPanel.audioCues[i] : ({})
    }
    // Air is whatever the status block says, never the press: this cue is
    // live only while the track on air names the same file.
    readonly property bool cueLive: worker.standaloneOnAir
                                    && !!outputPanel.selectedCue.sourcePath
                                    && outputPanel.selectedCue.sourcePath === worker.standalonePath
    readonly property bool cueRefused: {
        const cue = outputPanel.selectedCue
        return worker.standaloneRefusal !== "" && !!cue.sourcePath
               && cue.sourcePath === worker.standaloneRefusalPath
    }
    readonly property bool standaloneMuted: {
        for (const s of worker.mediaSources) if (s.id === "standalone") return !!s.muted
        return false
    }
    // The one sentence this area says about audio. Its order is the
    // operator's: a track that died mid-air first, then what is actually on
    // air, then what stands between the selection and sound.
    readonly property string audioStatus: {
        if (worker.standaloneError !== "")
            return "cannot play audio: " + outputPanel.fileName(worker.standalonePath)
        if (worker.standaloneOnAir) {
            const pos = worker.standalonePosition >= 0
                        ? " \u00b7 " + worker.standalonePosition.toFixed(1) + " s" : ""
            return outputPanel.fileName(worker.standalonePath) + " on air" + pos
                   + (outputPanel.standaloneMuted ? " \u00b7 muted" : "")
        }
        if (outputPanel.audioCues.length === 0)
            return "no prepared audio \u2014 import it in the media bin"
        if (!worker.connected) return "no output connected \u2014 nothing can play"
        if (outputPanel.cueRefused)
            return "cannot play " + outputPanel.fileName(outputPanel.selectedCue.sourcePath)
        return "pick a cue and press Play"
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

    // One source of the acknowledged mix: gain and mute as the worker
    // reports them, sent back as media.gain/media.mute and then confirmed
    // by the next status block — a refused change snaps the control back.
    component SourceMix: Column {
        required property var source
        property string label: ""
        spacing: 2
        Text { text: SourceMix.label; color: "#b0b0b0"; font.pixelSize: 11 }
        Row {
            width: parent.width
            spacing: 6
            Slider {
                id: gainSlider
                width: Math.max(60, parent.width - muteCheck.width - 6)
                from: 0; to: 1
                value: SourceMix.source ? SourceMix.source.gain : 0
                onMoved: worker.setMediaGain(SourceMix.source.id, value)
                Connections {
                    target: worker
                    function onMediaChanged() {
                        if (!gainSlider.pressed && SourceMix.source)
                            gainSlider.value = SourceMix.source.gain
                    }
                }
            }
            CheckBox {
                id: muteCheck
                text: "Mute"; font.pixelSize: 12
                checked: SourceMix.source ? !!SourceMix.source.muted : false
                onToggled: worker.setMediaMute(SourceMix.source.id, checked)
            }
        }
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
            Rectangle {
                id: previewBox
                objectName: "audiencePreview"
                width: monitorRow.width - clears.width - monitorRow.spacing
                height: width * 9 / 16
                color: "black"
                border.color: "#505050"
                PreviewView { anchors.fill: parent; anchors.margins: 1; controller: worker }
                Text {
                    objectName: "previewOfflineNote"
                    anchors.centerIn: parent
                    visible: !worker.outputLive
                    color: "#aaaaaa"; font.pixelSize: 12
                    text: "Output disconnected"
                }
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
                    enabled: worker.wordsOnAir || worker.backgroundOnAir || worker.canRestore
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
                : (worker.wordsOnAir || worker.backgroundOnAir ? "\u25cf  Output active" : "\u25cb  Output clear")
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
                        color: "#b0b0b0"; font.pixelSize: 12
                        text: worker.mediaPosition < 0 ? "\u2014"
                            : worker.mediaPosition.toFixed(1) + " s"
                              + (worker.mediaDuration > 0
                                 ? " / " + worker.mediaDuration.toFixed(1) + " s" : "")
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
                                if (!seekSlider.pressed) seekSlider.value = worker.mediaPosition
                            }
                        }
                    }
                }
                Repeater {
                    model: worker.mediaSources.filter(s => s.id !== "standalone")
                    delegate: SourceMix {
                        required property var modelData
                        source: modelData
                        label: modelData.id === "video" ? "Video audio" : modelData.id
                        width: parent.width
                    }
                }

                Rectangle { width: parent.width; height: 1; color: "#505050" }

                // ---------------- audio ----------------
                // The independent production track: prepared cues, explicit
                // activation, acknowledged air. A slide change moves none of
                // this — the track has its own life, which is why it is a
                // cue and not a slide's accessory.
                Text {
                    objectName: "audioHeading"
                    text: "AUDIO TRACK"; color: "#bbbbbb"; font.pixelSize: 11; font.bold: true
                }
                ComboBox {
                    id: audioCueCombo
                    objectName: "audioCueCombo"
                    width: parent.width
                    visible: outputPanel.audioCues.length > 0
                    textRole: "name"
                    model: outputPanel.audioCues
                    // Deliberately no onActivated: choosing from a list is
                    // browsing, and browsing sends nothing to an output.
                }
                Row {
                    spacing: 6
                    visible: outputPanel.audioCues.length > 0
                    PanelButton {
                        objectName: "audioPlayButton"
                        text: "Play"
                        // The same enablement honesty as the bin's cue: an
                        // output must exist to receive the track; whether
                        // the FILE plays is the probe's answer, so a cue is
                        // always worth asking with. Already-live cues are
                        // Stop's business, not a re-send.
                        enabled: worker.connected && !!outputPanel.selectedCue.sourcePath
                                 && !outputPanel.cueLive
                        Accessible.name: "Play audio cue"
                            + (worker.connected ? "" : " \u2014 no output connected")
                        onClicked: worker.selectStandaloneTrack(outputPanel.selectedCue.sourcePath)
                    }
                    PanelButton {
                        objectName: "audioStopButton"
                        text: "Stop"
                        enabled: worker.standaloneOnAir
                        Accessible.name: "Stop audio track"
                        onClicked: worker.selectStandaloneTrack("")
                    }
                }
                Text {
                    objectName: "audioStatusLine"
                    width: parent.width
                    wrapMode: Text.WordWrap; font.pixelSize: 11
                    color: outputPanel.cueLive || worker.standaloneOnAir ? "#5ec96a"
                           : worker.standaloneError !== "" || outputPanel.cueRefused
                             ? "#d9a441" : "#929292"
                    text: outputPanel.audioStatus
                }
                Repeater {
                    model: worker.mediaSources.filter(s => s.id === "standalone")
                    delegate: SourceMix {
                        required property var modelData
                        source: modelData
                        label: "Track audio"
                        width: parent.width
                    }
                }
            }
        }
    }
}
