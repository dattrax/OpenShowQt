import QtQuick
import QtQuick.Controls

/*
 * The Output layers dialog (milestone FO, task FO.4b).
 *
 * ROWS are production layers, COLUMNS are destinations — never one global set
 * of layer ticks disguised as three outputs (FO.0 §5). Each column has its
 * own enable control and route; Background/Images/Words are legal everywhere,
 * "Next cue" and "Notes" are stage-only cells and are visibly disabled
 * elsewhere rather than accepted and ignored.
 *
 * Apply edits a DRAFT matrix and submits the whole answer once; Cancel writes
 * nothing. The draft starts from the machine's stored masks. Checking a cell
 * never starts an output: `enabled` is session state and is deliberately not
 * persisted, so a restart begins with every destination off (FO.0 §4.3).
 * The stage window and the NDI sender are their own implemented leaves; until
 * one exists the dialog says so instead of pretending a tick put words on a
 * screen.
 */
Dialog {
    id: layersDialog
    objectName: "outputLayersDialog"

    property var draft: ({})
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
    width: Math.min(780, (parent ? parent.width : 900) - 60)
    x: Math.max(0, (parent ? parent.width : 900 - width) / 2 - width / 2)
    y: Math.max(0, (parent ? parent.height : 700) / 2 - height / 2)

    function column(key) {
        return layersDialog.draft[key] || ({enabled: false, layers: []})
    }
    function hasCell(key, layer) {
        return layersDialog.column(key).layers.indexOf(layer) >= 0
    }
    function cellEditable(key, layer) {
        return key === "stage" || layer !== "next" && layer !== "notes"
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
    function setEnabled(key, on) {
        layersDialog.mutate(function (next) {
            if (next[key] === undefined)
                next[key] = {enabled: false, layers: []}
            next[key].enabled = on
        })
    }
    function loadDraft() {
        layersDialog.draft = JSON.parse(appSettings.outputRoutingJson())
        // The audience window is this application's own output and is
        // already live; its column reads as on so Apply never blanks the
        // operator's screen by accident. Stage/broadcast always start off.
        if (layersDialog.draft.audience !== undefined)
            layersDialog.draft.audience.enabled = true
        audienceMonitor.currentIndex = indexOfMonitor(appSettings.audienceMonitor)
        stageMonitor.currentIndex = indexOfMonitor(appSettings.stageMonitor)
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
        appSettings.audienceMonitor = monitorIndexAt(audienceMonitor.currentIndex, -1)
        appSettings.stageMonitor = monitorIndexAt(stageMonitor.currentIndex, -1)
        appSettings.broadcastSource = sourceField.text
        const json = JSON.stringify(layersDialog.draft)
        if (!appSettings.applyOutputRouting(json))
            return  // the refusal stays visible; the last accepted matrix is kept
        worker.applyOutputRouting(json)
        layersDialog.close()
    }

    onOpened: loadDraft()

    contentItem: Column {
        spacing: 10
        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            color: "#b0b0b0"
            font.pixelSize: 11
            text: "Each column is one destination. Checking a cell never turns an output on; "
                  + "enabling a column is the operator's explicit act, and every destination "
                  + "starts off after a restart."
        }

        // Header row: the layer vocabulary on the left, one column per
        // destination with its enable control above it.
        Row {
            width: parent.width
            spacing: 6
            Item { width: 170; height: 1 }
            Repeater {
                model: layersDialog.targets
                delegate: Column {
                    required property var modelData
                    width: (layersDialog.width - 230) / 3 - 6
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
                        Accessible.name: "Enable " + modelData.label
                        text: "Enabled"
                        font.pixelSize: 11
                        checked: layersDialog.column(modelData.key).enabled === true
                        onToggled: layersDialog.setEnabled(modelData.key, checked)
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
                    width: 170
                    text: modelData.label
                    color: modelData.stageOnly ? "#c9b37a" : "#b0b0b0"
                    font.pixelSize: 12
                    elide: Text.ElideRight
                }
                Repeater {
                    model: layersDialog.targets
                    delegate: Item {
                        required property var modelData
                        width: (layersDialog.width - 230) / 3 - 6
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
                width: 170
                text: "Route"
                color: "#b0b0b0"
                font.pixelSize: 12
            }
            Column {
                width: (layersDialog.width - 230) / 3 - 6
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
                width: (layersDialog.width - 230) / 3 - 6
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
                width: (layersDialog.width - 230) / 3 - 6
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
            text: "This build stores the matrix and the routes. The Stage HDMI window and the "
                  + "NDI sender are separate, physically verified leaves; until they are on, "
                  + "enabling a column is recorded but nothing leaves this machine."
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

        Row {
            anchors.right: parent.right
            spacing: 8
            PanelButton {
                objectName: "outputLayersCancel"
                text: "Cancel"
                onClicked: {
                    layersDialog.loadDraft()
                    layersDialog.close()
                }
            }
            PanelButton {
                objectName: "outputLayersApply"
                text: "Apply"
                highlighted: true
                onClicked: layersDialog.apply()
            }
        }
    }
}
