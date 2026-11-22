import QtQuick
import QtQuick.Controls

/*
 * The Output layers dialog (milestone FO, task FO.4b).
 *
 * ROWS are production layers, COLUMNS are destinations — never one global set
 * of layer ticks disguised as three outputs (FO.0 §5). Each column has its
 * own availability indicator and route; Background/Images/Words are legal everywhere,
 * "Next cue" and "Notes" are stage-only cells and are visibly disabled
 * elsewhere rather than accepted and ignored.
 *
 * Apply edits a DRAFT matrix and submits the whole answer once; Cancel writes
 * nothing. The draft starts from the machine's stored masks. Checking a cell
 * never starts an output: `enabled` is session state and is deliberately not
 * persisted, so a restart begins with every destination off (FO.0 §4.3).
 * Stage and NDI runtime outputs are not implemented yet. Their masks can be
 * edited for later, but their enable controls must not offer activation.
 */
Dialog {
    id: layersDialog
    objectName: "outputLayersDialog"

    property var draft: ({})
    property string localError: ""
    readonly property var targets: [
        {key: "audience", label: "Audience HDMI", route: "monitor"},
        {key: "stage", label: "Stage HDMI", route: "monitor"},
        {key: "broadcast", label: "NDI lower thirds", route: "source"}
    ]
    readonly property var layers: [
        {key: "background", label: "Background", stageOnly: false},
        {key: "images", label: "Images", stageOnly: false},
        {key: "words", label: "Words / current text", stageOnly: false},
        {key: "next", label: "Next cue", stageOnly: true},
        {key: "notes", label: "Notes", stageOnly: true}
    ]

    title: "Output layers"
    modal: true
    standardButtons: Dialog.NoButton
    // The caller lives in the narrow output dock. Popup geometry must use
    // the window overlay or the matrix ends up narrower than its row labels.
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(780, parent ? parent.width - 32 : 780)
    height: Math.min(implicitHeight, parent ? parent.height - 32 : implicitHeight)
    closePolicy: Popup.CloseOnEscape
    background: Rectangle {
        color: "#353535"
        radius: 8
        border.color: "#606060"
    }

    function column(key) {
        return layersDialog.draft[key] || ({enabled: false, layers: []})
    }
    function hasCell(key, layer) {
        return layersDialog.column(key).layers.indexOf(layer) >= 0
    }
    function cellEditable(key, layer) {
        if (layer === "notes")
            return false  // no production notes source yet
        if (key === "stage")
            return true
        return layer !== "next"
    }
    function columnEditable(key) {
        if (key === "stage")
            return worker.stageCapable === true
        return false
    }
    function columnStatus(key) {
        if (key === "audience")
            return worker.outputLive ? "Available" : "Waiting for worker"
        if (key === "stage") {
            if (!worker.stageCapable)
                return "Unavailable"
            if (worker.stageError !== "")
                return worker.stageError
            if (worker.stageActive)
                return "On air \u2014 " + (worker.stageMonitorName || "stage display")
            return worker.stageEnabled ? "Starting\u2026" : "Available"
        }
        return "Unavailable"
    }
    function toggleColumn(key, on) {
        layersDialog.mutate(function (next) {
            if (next[key] === undefined)
                next[key] = {enabled: false, layers: []}
            next[key].enabled = on
        })
    }
    function mutate(change) {
        const next = JSON.parse(JSON.stringify(layersDialog.draft))
        change(next)
        layersDialog.draft = next
    }
    function setCell(key, layer, on) {
        layersDialog.mutate(function (next) {
            if (next[key] === undefined)
                next[key] = {enabled: false, layers: []}
            const at = next[key].layers.indexOf(layer)
            if (on && at < 0)
                next[key].layers.push(layer)
            if (!on && at >= 0)
                next[key].layers.splice(at, 1)
        })
    }
    function loadDraft() {
        const saved = JSON.parse(appSettings.outputRoutingJson)
        // The audience window is this application's own output and is
        // already live; its column reads as on. Settings can also change
        // its layers, so start with the acknowledged state when connected.
        if (saved.audience !== undefined) {
            saved.audience.enabled = true
            if (worker.outputLive)
                saved.audience.layers = ["background", "images", "words"].filter(
                    layer => worker.layers[layer] === true)
        }
        // Stage enablement is SESSION state (FO.0 §4.3): the draft starts from
        // the worker's observed answer, never from a saved `enabled` flag, so
        // reopening the dialog or restarting cannot put the stage on air.
        if (saved.stage === undefined)
            saved.stage = {enabled: false, layers: ["words", "next"]}
        saved.stage.enabled = worker.stageEnabled
        layersDialog.draft = saved
        audienceMonitor.currentIndex = indexOfMonitor(worker.outputLive
                                                      ? worker.currentMonitor
                                                      : appSettings.audienceMonitor)
        stageMonitor.currentIndex = indexOfMonitor(appSettings.stageMonitor)
        // FO.5a.2: a saved index is a preference, not an identity. If the
        // panel that index named has been replaced/reordered, do not quietly
        // point Stage at whatever now occupies it.
        if (appSettings.stageMonitor >= 0 && appSettings.stageMonitorIdentity !== "") {
            const row = worker.displays.find(d => d.index === appSettings.stageMonitor)
            if (!row || row.identity !== appSettings.stageMonitorIdentity) {
                stageMonitor.currentIndex = 0
                layersDialog.localError =
                    "The saved stage display is not attached; choose its display again."
            }
        }
        sourceField.text = appSettings.broadcastSource
    }
    function indexOfMonitor(index) {
        const rows = [{index: -1}].concat(worker.displays)
        for (let i = 0; i < rows.length; ++i)
            if (rows[i].index === index)
                return i
        return 0
    }
    function monitorIndexAt(at, fallback) {
        const rows = [{index: -1}].concat(worker.displays)
        return at >= 0 && at < rows.length ? rows[at].index : fallback
    }
    function apply() {
        layersDialog.localError = ""
        const json = JSON.stringify(layersDialog.draft)
        if (!appSettings.applyOutputRouting(json))
            return  // the refusal stays visible; the last accepted matrix is kept
        appSettings.audienceMonitor = monitorIndexAt(audienceMonitor.currentIndex, -1)
        appSettings.stageMonitor = monitorIndexAt(stageMonitor.currentIndex, -1)
        const stageRow = worker.displays.find(d => d.index === appSettings.stageMonitor)
        appSettings.stageMonitorIdentity = stageRow ? stageRow.identity : ""
        appSettings.broadcastSource = sourceField.text
        const stageOn = layersDialog.column("stage").enabled === true
        if (stageOn && appSettings.stageMonitor < 0) {
            layersDialog.localError = "Choose an attached display before enabling Stage."
            return
        }
        if (stageOn && !worker.stageCapable) {
            layersDialog.localError = "This worker does not drive a Stage HDMI output."
            return
        }
        if (worker.outputLive) {
            worker.applyOutputRouting(json)
            if (appSettings.audienceMonitor !== worker.currentMonitor)
                worker.selectDisplay(appSettings.audienceMonitor)
        }
        if (worker.stageCapable)
            worker.configureStage(stageOn, appSettings.stageMonitor,
                                  layersDialog.column("stage").layers)
        layersDialog.close()
    }

    onAboutToShow: loadDraft()

    contentItem: ScrollView {
        id: scroll
        objectName: "outputLayersScroll"
        implicitHeight: matrix.implicitHeight
        contentWidth: Math.max(680, availableWidth)
        clip: true

        Column {
            id: matrix
            width: scroll.contentWidth
            readonly property real labelWidth: 170
            readonly property real columnWidth: (width - labelWidth - 18) / 3
            spacing: 10
            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                color: "#b0b0b0"
                font.pixelSize: 11
                text: "Each column is one destination. Apply updates the audience layers and display. "
                      + "Stage and NDI layer selections are saved for later."
            }

            // Header row: the layer vocabulary on the left, one column per
            // destination with its availability above it.
            Row {
                width: parent.width
                spacing: 6
                Item { width: matrix.labelWidth; height: 1 }
                Repeater {
                    model: layersDialog.targets
                    delegate: Column {
                        required property var modelData
                        width: matrix.columnWidth
                        spacing: 4
                        Text {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            text: modelData.label
                            color: "#dddddd"
                            font.pixelSize: 12
                            font.bold: true
                        }
                        CheckBox {
                            objectName: "outputColumnEnable-" + modelData.key
                            Accessible.name: modelData.label + (columnEditable(modelData.key)
                                                               ? " enable"
                                                               : " unavailable")
                            text: columnStatus(modelData.key)
                            font.pixelSize: 11
                            enabled: columnEditable(modelData.key)
                            checked: layersDialog.column(modelData.key).enabled === true
                            onToggled: layersDialog.toggleColumn(modelData.key, checked)
                        }
                    }
                }
            }

            // One row per layer; unsupported cells are disabled, never hidden and
            // never accepted-and-ignored.
            Repeater {
                model: layersDialog.layers
                delegate: Row {
                    required property var modelData
                    id: layerRow
                    width: parent.width
                    spacing: 6
                    Text {
                        width: matrix.labelWidth
                        text: modelData.label
                        color: modelData.stageOnly ? "#c9b37a" : "#b0b0b0"
                        font.pixelSize: 12
                        elide: Text.ElideRight
                    }
                    Repeater {
                        model: layersDialog.targets
                        delegate: Item {
                            required property var modelData
                            width: matrix.columnWidth
                            height: 30
                            CheckBox {
                                id: cell
                                objectName: "outputCell-" + modelData.key + "-" + layerRow.modelData.key
                                anchors.centerIn: parent
                                Accessible.name: modelData.label + ": " + layerRow.modelData.label
                                enabled: layersDialog.cellEditable(modelData.key,
                                                                   layerRow.modelData.key)
                                checked: layersDialog.hasCell(modelData.key,
                                                              layerRow.modelData.key)
                                onToggled: layersDialog.setCell(modelData.key,
                                                                layerRow.modelData.key, checked)
                            }
                            Text {
                                anchors.centerIn: parent
                                visible: !cell.enabled
                                text: "\u2014"
                                color: "#7a7a7a"
                                font.pixelSize: 12
                            }
                        }
                    }
                }
            }

            // Routes: a display per HDMI column, a source name for NDI.
            Row {
                width: parent.width
                spacing: 6
                Text {
                    width: matrix.labelWidth
                    text: "Route"
                    color: "#b0b0b0"
                    font.pixelSize: 12
                }
                Column {
                    width: matrix.columnWidth
                    spacing: 2
                    ComboBox {
                        id: audienceMonitor
                        objectName: "outputMonitorAudience"
                        width: parent.width
                        model: [{index: -1, label: "Windowed"}].concat(
                                   worker.displays.map(d => ({index: d.index,
                                                              label: d.name + " — " + d.size})))
                        textRole: "label"
                        Accessible.name: "Audience display"
                    }
                }
                Column {
                    width: matrix.columnWidth
                    spacing: 2
                    ComboBox {
                        id: stageMonitor
                        objectName: "outputMonitorStage"
                        width: parent.width
                        model: [{index: -1, label: "Not chosen"}].concat(
                                   worker.displays.map(d => ({index: d.index,
                                                              label: d.name + " — " + d.size})))
                        textRole: "label"
                        Accessible.name: "Stage display"
                    }
                }
                Column {
                    width: matrix.columnWidth
                    spacing: 2
                    PanelField {
                        id: sourceField
                        objectName: "outputSourceName"
                        width: parent.width
                        Accessible.name: "NDI source name"
                    }
                }
            }

            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                color: "#d9a441"
                font.pixelSize: 11
                objectName: "outputDestinationsUnavailable"
                text: worker.stageCapable
                      ? "Stage HDMI is available on the worker. Enabling it opens a second "
                        + "borderless output on the chosen display. NDI output is not implemented "
                        + "in this build; its saved layers and source name are kept for later."
                      : "Stage HDMI and NDI output are not available on this worker. Connecting "
                        + "another display does not enable Stage HDMI by itself. Their saved layers "
                        + "and routes will be used when those outputs are available."
            }
            Text {
                objectName: "outputStageStatus"
                width: parent.width
                visible: worker.stageCapable === true
                wrapMode: Text.WordWrap
                color: (worker.stageError || "") !== "" ? "#e08080" : "#9fbf9f"
                font.pixelSize: 11
                text: {
                    if ((worker.stageError || "") !== "")
                        return "Stage: " + worker.stageError
                    if (!worker.stageEnabled)
                        return "Stage: disabled"
                    if (!worker.stageActive)
                        return "Stage: starting on " + (worker.stageMonitorName || "the chosen display")
                    return "Stage: on air at " + worker.stageFrames + " frames"
                           + (worker.stageNext ? ", next cue shown" : ", no next cue")
                           + (worker.stageOverlap ? " \u2014 current box overlaps the next slot" : "")
                }
            }
            Text {
                objectName: "outputStageLocalError"
                width: parent.width
                visible: layersDialog.localError !== ""
                wrapMode: Text.WordWrap
                color: "#e08080"
                font.pixelSize: 11
                text: layersDialog.localError
            }
            Text {
                objectName: "outputRoutingStatus"
                width: parent.width
                visible: worker.connected && worker.outputRoutingStatus !== ""
                         && worker.outputRoutingStatus !== "accepted"
                         && worker.outputRoutingStatus !== "pending"
                wrapMode: Text.WordWrap
                color: "#d9a441"
                font.pixelSize: 11
                text: "Worker: " + worker.outputRoutingStatus
            }
            Text {
                objectName: "outputLayersError"
                width: parent.width
                visible: appSettings.outputRoutingError !== ""
                wrapMode: Text.WordWrap
                color: "#e08080"
                font.pixelSize: 11
                text: appSettings.outputRoutingError
            }

        }
    }

    footer: DialogButtonBox {
        PanelButton {
            objectName: "outputLayersCancel"
            text: "Cancel"
            focusPolicy: Qt.StrongFocus
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            onClicked: layersDialog.close()
        }
        PanelButton {
            objectName: "outputLayersApply"
            text: "Apply"
            highlighted: true
            focusPolicy: Qt.StrongFocus
            DialogButtonBox.buttonRole: DialogButtonBox.ApplyRole
            onClicked: layersDialog.apply()
        }
    }
}
