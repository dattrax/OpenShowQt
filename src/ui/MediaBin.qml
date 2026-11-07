import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs

// UI.4a: the bottom of the window is the MEDIA BIN. The presentation list
// this drawer used to carry is gone — the sidebar's collection outline is the
// one place a presentation is found (UI.2), and a second copy here meant two
// search boxes, two selection states and two sets of rules for one library.
//
// What the bin is: the service's registered media as named cues, with the
// kind each one has (DocumentViews writes it into every asset row from the
// domain's one name rule), collections down the left that filter that list,
// and import kept separate from every activation. The two controls that
// change the document (Remove, Add to slide) stay behind editingEnabled; a
// file the product cannot present is still listed and says so, because
// hiding the row would leave the operator wondering where the import went.
//
// UI.4b adds the activation, and it is one control on one kind. What the
// worker offers at runtime is the independent production audio track
// (media.standalone): a file with a life of its own, unaffected by slide
// changes, which is exactly what a bin cue is — so an audio row gets
// Play/Stop. A video has no cue of its own to fire: its route to the audience
// is as a slide's background, an authored change made in Edit. A picture's
// route is a slide. Those kinds are told where they go rather than given a
// button with nothing behind it, per the plan's rule that a control appears
// only when its complete action exists. Activation touches no document: no
// undo step, no moved cursor, nothing presented. And it never promises a
// file will play — the worker probes the bytes in its own file thread and
// answers, and until that answer says so this file says nothing.
//
// The 40px bar is the toggle (the reference's strip click), the top edge
// drags the height (WorkspacePanels' heightFor does the snapping), and the
// audience monitor to the right of this area is never covered by it.
FocusScope {
    id: mediaBin

    Rectangle { anchors.fill: parent; color: "#353535"; z: -1 }
    height: panels.drawerHeight
    property bool editingEnabled: false
    clip: true

    readonly property bool open: height > 40.5
    // "" is every row; anything else is one kind from DocumentViews.
    property string filter: ""
    property string selectedAssetId: ""

    readonly property var kindLabels: ({
        picture: "Pictures", video: "Video", audio: "Audio",
        camera: "Cameras", other: "Other"
    })
    // One row per kind the library actually holds, each with its count, and
    // "All media" first: it is the view the operator widens back out to, and
    // the only one an empty library has. These are the collections the bin can
    // honour — every one is a filter over rows whose type the product already
    // knows — and a kind with no assets is absent rather than a dead click.
    readonly property var collections: {
        const counts = ({picture: 0, video: 0, audio: 0, camera: 0, other: 0})
        for (const asset of docs.assets) {
            if (counts[asset.kind] === undefined) counts[asset.kind] = 0
            counts[asset.kind]++
        }
        const list = [{kind: "", label: "All media", count: docs.assets.length}]
        for (const kind of ["picture", "video", "audio", "camera", "other"]) {
            if (counts[kind] > 0) {
                list.push({kind: kind, label: mediaBin.kindLabels[kind],
                           count: counts[kind]})
            }
        }
        return list
    }
    readonly property var rows: mediaBin.filter === ""
        ? docs.assets
        : docs.assets.filter(a => a.kind === mediaBin.filter)
    // ------------------------------------------------------------------
    // The notice rule (UI.4b), in one place, with its inputs as arguments
    // rather than bindings: the suite drives this table directly, because an
    // offscreen fixture cannot conjure a live transport. noticeFor(row) below
    // is exactly this, with the worker's real answers.
    //
    // The order is what the operator acts on. A file the audience is hearing
    // is the present fact. Next, a file the product could use that the
    // document says is not on disk — worth saying before anyone reaches for a
    // button, though the button stays live because this view only took a
    // snapshot and the worker's probe is the authority. Then the standing kind
    // rules for the rows nothing fires from here, which no file's state would
    // change. Only an audio row that is present and refused by that probe says
    // "cannot play", and an audio row that is fine says nothing, because its
    // Play button is the notice. Each string stays short enough to read whole
    // in the row's notice column (the suite checks that); the long version of
    // a refusal is an Edit detail, and Show names the file.
    // ------------------------------------------------------------------
    function noticeForState(kind, present, live, refused) {
        if (!kind) return ""
        if (live) return "on air"
        // A file the product COULD use, and that is not there: the one thing
        // worth saying. "other" is exempt — its type is unusable either way,
        // and the reason below is the one that stops an operator trying.
        if (!present && kind !== "other") return "file is not here"
        if (kind === "video") return "background only \u2014 set it in Edit"
        if (kind === "camera") return "live NDI \u2014 set it as a background in Edit"
        if (kind === "other") return "nothing presents this file type"
        if (kind === "audio" && refused) return "cannot play"
        return ""
    }
    // Whether the worker says THIS file is what the audience is getting: the
    // production track for an audio row, the on-air slide's background for a
    // video. Never derived from a click.
    function isLive(row) {
        if (!row || !row.sourcePath) return false
        if (row.kind === "audio")
            return worker.standaloneOnAir && row.sourcePath === worker.standalonePath
        if (row.kind === "video")
            return worker.mediaOnAir && row.sourcePath === worker.mediaPath
        if (row.kind === "camera")
            return worker.mediaOnAir && row.cameraSource === worker.mediaPath
        return false
    }
    function isRefused(row) {
        return worker.standaloneRefusal !== "" && !!row && !!row.sourcePath
                && row.sourcePath === worker.standaloneRefusalPath
    }
    function noticeFor(row) {
        if (!row || !row.kind) return ""
        return mediaBin.noticeForState(row.kind, row.exists !== false,
                                       mediaBin.isLive(row), mediaBin.isRefused(row))
    }
    // Live is a marker, not a warning; everything else in this column is. The
    // brighter pair is for a selected row, whose background is the blue.
    function noticeColor(row, onSelection) {
        const live = mediaBin.isLive(row)
        if (onSelection) return live ? "#c8f5cd" : "#ffe0a3"
        return live ? "#5ec96a" : "#c8a24a"
    }
    // The one cue the bin can activate: an audio row, while an output is
    // connected to receive it. Whether the FILE is there is deliberately not
    // part of enablement — that is a snapshot this view took, and a row marked
    // "file is not here" whose file came back would stay dead until something
    // re-read the document. The worker's probe is the authority on a file, so
    // asking is always allowed and Play is never a promise.
    readonly property bool canActivate: selected.kind === "audio" && worker.connected
    readonly property bool selectedLive: mediaBin.isLive(mediaBin.selected)
    // The row's whole story, for the info column. Kept to the file's name
    // where one exists (the plan's rule: filename on Show, the detailed
    // failure text down the Edit side) — and a claim about playback appears
    // here only in the worker's own voice.
    readonly property string statusLine: {
        const row = mediaBin.selected
        if (!row || !row.kind) return ""
        if (row.kind === "audio" && mediaBin.selectedLive) {
            return "audio track on air"
                   + (worker.standalonePosition >= 0
                      ? " \u00b7 " + worker.standalonePosition.toFixed(1) + " s" : "")
                   + " \u00b7 loops until stopped"
        }
        if (row.exists === false && row.kind !== "other")
            return "this file is not where the service says it is"
        if (row.kind === "picture") return "a picture goes on a slide \u2014 Edit only"
        if (row.kind === "video") {
            return mediaBin.selectedLive
                   ? "on air as this slide\u2019s background"
                   : "a video goes to output as a slide\u2019s background \u2014 set it in Edit"
        }
        if (row.kind === "camera") {
            return mediaBin.selectedLive
                   ? "live on air as this slide\u2019s background"
                   : "live NDI source \u2014 set it as a background in Edit"
        }
        if (row.kind === "other") return "nothing presents this file type"
        // An audio cue, off air: what is standing between this row and sound.
        if (!worker.connected) return "no output connected \u2014 nothing can play"
        if (mediaBin.isRefused(row))
            return "cannot play audio: " + mediaBin.fileName(row.sourcePath)
        if (worker.standaloneOnAir) return "replaces the track now on air"
        return "press Play to start it on the production audio track"
    }
    // Camera cards wear 1..4 in creation order, whatever the current filter
    // shows. The model caps the list at four, so the numeral always has a slot.
    function cameraNumber(assetId) {
        const cameras = docs.assets.filter(a => a.kind === "camera")
        const index = cameras.findIndex(a => a.id === assetId)
        return index >= 0 ? index + 1 : 0
    }
    function fileName(path) {
        const parts = String(path).split("/")
        return parts[parts.length - 1]
    }
    // The selected row, whether or not the current filter shows it: the info
    // column describes what the operator last picked, and a filter change
    // does not silently change what is being described.
    readonly property var selected:
        docs.assets.filter(a => a.id === mediaBin.selectedAssetId)[0] || ({})

    // ---------------------------------------------------------------------
    // The 40px bar. Visible open or closed; clicking it toggles.
    // ---------------------------------------------------------------------
    Item {
        id: bar
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 40

        MouseArea {
            anchors.fill: parent
            onClicked: panels.toggleDrawer()
        }

        Row {
            anchors.left: parent.left
            anchors.leftMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8

            Text {
                anchors.verticalCenter: parent.verticalCenter
                font.pixelSize: 12
                font.bold: true
                color: "#eeeeee"
                text: "MEDIA"
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                font.pixelSize: 11
                color: "#b0b0b0"
                text: docs.assets.length + (docs.assets.length === 1 ? " item" : " items")
                      + (mediaBin.filter === "" ? ""
                         : " \u00b7 " + mediaBin.rows.length + " shown")
                      // The bin's one runtime control needs an output, and the
                      // bar is where the operator can see it has none even with
                      // the bin closed (the button itself says why when picked).
                      + (worker.connected ? "" : " \u00b7 output offline")
            }
        }
    }

    // ---------------------------------------------------------------------
    // The body: collections, the media list, and the item-info column.
    // ---------------------------------------------------------------------
    Item {
        id: body
        anchors.top: bar.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: 10
        anchors.rightMargin: 10
        anchors.bottomMargin: 10
        visible: mediaBin.open
        clip: true

        Rectangle {
            id: infoColumn
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.right: parent.right
            width: Math.min(220, parent.width * 0.28)
            color: "#242424"  // --primary-darkest
            border.color: "#4a4a4a"

            Column {
                anchors.fill: parent
                anchors.margins: 10
                spacing: 6

                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    color: "#b0b0b0"
                    font.pixelSize: 11
                    font.bold: true
                    text: "MEDIA INFO"
                }
                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    color: "#eeeeee"
                    font.pixelSize: 13
                    elide: Text.ElideMiddle
                    text: mediaBin.selected.name || "nothing selected"
                }
                Text {
                    objectName: "mediaInfoKind"
                    width: parent.width
                    wrapMode: Text.WordWrap
                    color: "#b0b0b0"
                    font.pixelSize: 11
                    text: mediaBin.selected.kind
                          ? (mediaBin.kindLabels[mediaBin.selected.kind]
                             + (mediaBin.noticeFor(mediaBin.selected) === ""
                                ? "" : " \u00b7 " + mediaBin.noticeFor(mediaBin.selected)))
                          : "Pick a media item to inspect it"
                }
                // What can be DONE with it, from here (UI.4b). The row's notice
                // column is a handful of words; this is the sentence, and it is
                // the same rule the notice and the button's enablement read.
                Text {
                    objectName: "mediaInfoStatus"
                    width: parent.width
                    wrapMode: Text.WordWrap
                    color: mediaBin.selectedLive ? "#5ec96a" : "#b0b0b0"
                    font.pixelSize: 11
                    text: mediaBin.statusLine
                }
                // The worker's own words about a refused cue. Show says the
                // file's name, per the plan's split for media failures; the
                // sentence that names the path is an Edit detail, exactly like
                // the path below it.
                Text {
                    objectName: "mediaInfoDetail"
                    width: parent.width
                    wrapMode: Text.WordWrap
                    visible: mediaBin.editingEnabled && mediaBin.isRefused(mediaBin.selected)
                    color: "#d9a441"
                    font.pixelSize: 10
                    text: "the output refused this file: " + worker.standaloneRefusal
                }
                // The path is an Edit detail (the Show page identifies media by
                // name); on Show there is no path text to squint at.
                Text {
                    objectName: "mediaInfoPath"
                    width: parent.width
                    wrapMode: Text.WrapAnywhere
                    visible: mediaBin.editingEnabled
                    color: "#b0b0b0"
                    font.pixelSize: 10
                    text: mediaBin.selected.sourcePath || ""
                }
                // The bin's controls live in the inspector, so the grid keeps
                // its full width and the top edges line up. MB.4 retires
                // Remove to the context menu and replaces Add to slide with a
                // drop; until then they stay here rather than over the grid.
                Column {
                    width: parent.width
                    spacing: 6
                    PanelButton {
                        objectName: "mediaActivateButton"
                        width: parent.width
                        visible: mediaBin.selected.kind === "audio"
                        enabled: mediaBin.canActivate
                        text: mediaBin.selectedLive ? "Stop" : "Play"
                        Accessible.name: text + (mediaBin.canActivate
                                                 ? "" : " \u2014 no output connected")
                        onClicked: worker.selectStandaloneTrack(
                            mediaBin.selectedLive ? "" : mediaBin.selected.sourcePath)
                    }
                    PanelButton {
                        objectName: "mediaRemoveButton"
                        width: parent.width
                        visible: mediaBin.editingEnabled
                        text: "Remove"
                        enabled: mediaBin.selectedAssetId !== ""
                        onClicked: {
                            docs.removeAsset(mediaBin.selectedAssetId)
                            mediaBin.selectedAssetId = ""
                        }
                    }
                    PanelButton {
                        objectName: "placeAssetButton"
                        width: parent.width
                        visible: mediaBin.editingEnabled
                        text: "Add to slide"
                        enabled: mediaBin.selected.kind === "picture"
                        onClicked: docs.placeAssetOnSlide(mediaBin.selectedAssetId)
                    }
                }
            }
        }

        // --- the collections down the left ---
        Rectangle {
            id: collectionsBox
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            width: Math.min(170, Math.max(120, parent.width * 0.18))
            color: "#242424"
            border.color: "#4a4a4a"

            ListView {
                id: collections
                objectName: "mediaCollections"
                anchors.fill: parent
                anchors.margins: 1
                clip: true
                model: mediaBin.collections

                delegate: Item {
                    id: collectionRow
                    objectName: "mediaCollectionRow"

                    required property var modelData
                    readonly property bool active: mediaBin.filter === modelData.kind
                    width: collections.width
                    height: 26
                    Accessible.name: modelData.label + ": " + modelData.count
                                     + (modelData.count === 1 ? " item" : " items")

                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: 1
                        color: collectionRow.active ? "#1685f8"
                             : (collectionMouse.containsMouse ? "#4a4a4a" : "transparent")
                    }
                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - 46
                        elide: Text.ElideRight
                        font.pixelSize: 12
                        color: collectionRow.active ? "#ffffff" : "#eeeeee"
                        text: modelData.label
                    }
                    Text {
                        anchors.right: parent.right
                        anchors.rightMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        font.pixelSize: 11
                        color: collectionRow.active ? "#d9ebff" : "#b0b0b0"
                        text: modelData.count
                    }
                    MouseArea {
                        id: collectionMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        acceptedButtons: Qt.LeftButton
                        onClicked: mediaBin.filter = collectionRow.modelData.kind
                    }
                }
            }
        }

        // --- the media itself ---
        Item {
            id: mediaArea
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.left: collectionsBox.right
            anchors.right: infoColumn.left
            anchors.leftMargin: 10
            anchors.rightMargin: 10

            Rectangle {
                anchors.fill: parent
                color: "#242424"
                border.color: "#4a4a4a"

                GridView {
                    id: mediaGrid
                    objectName: "mediaBinList"
                    anchors.fill: parent
                    anchors.margins: 1
                    clip: true
                    model: mediaBin.rows
                    // Roughly 170 px minimum per card; the columns reflow as
                    // the bin is resized and the inspector keeps its width.
                    cellWidth: Math.max(1, Math.floor(width / Math.max(1, Math.floor(width / 170))))
                    cellHeight: Math.round(cellWidth * 9 / 16) + 52
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar {}

                    delegate: MediaCard {
                        objectName: "mediaBinRow"
                        width: mediaGrid.cellWidth - 8
                        height: mediaGrid.cellHeight - 8
                        kindLabel: mediaBin.kindLabels[modelData.kind] || ""
                        notice: mediaBin.noticeFor(modelData)
                        sourcePath: modelData.sourcePath
                        cameraNumber: mediaBin.cameraNumber(modelData.id)
                        selected: modelData.id === mediaBin.selectedAssetId
                        onActivated: mediaBin.selectedAssetId = modelData.id
                    }
                }

                Text {
                    anchors.centerIn: parent
                    visible: mediaBin.rows.length === 0
                    color: "#929292"
                    font.pixelSize: 13
                    horizontalAlignment: Text.AlignHCenter
                    width: parent.width
                    text: docs.assets.length === 0
                          ? "no media \u2014 import a file"
                          : "no " + mediaBin.kindLabels[mediaBin.filter]
                            + " \u2014 choose All media to see the rest"
                }
            }
        }
    }
    FileDialog {
        id: mediaDialog
        title: "Import media"
        modality: Qt.ApplicationModal
        // The patterns come from the domain's extension table, so the dialog
        // cannot offer a name the bin would call Other while withholding one
        // it would recognise (and "All files" stays, because registering
        // something unsupported is possible and the row says so).
        nameFilters: docs.mediaImportNameFilters()
        onAccepted: docs.importMediaUrl(selectedFile)
    }

    // ---------------------------------------------------------------------
    // The top edge drags the height.
    // ---------------------------------------------------------------------
    MouseArea {
        id: edgeDrag
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 6
        cursorShape: Qt.SizeVerCursor
        property real pressOffset: 0

        onPressed: function (mouse) {
            const y = mapToItem(null, 0, mouse.y).y
            pressOffset = mediaBin.parent.height - mediaBin.height - y
        }
        onPositionChanged: function (mouse) {
            if (!pressed) return
            const y = mapToItem(null, 0, mouse.y).y
            // maxHeight is the window minus the 40px top bar, like the
            // reference's available height.
            panels.resizeDrawer(mediaBin.parent.height - y - pressOffset,
                                mediaBin.parent.height - 64 - 300)
        }
    }

    // ---------------------------------------------------------------------
    // ND.7: right-click the bin to add a discovered NDI camera. The action
    // is present only when the worker's hello named live.sources.v1 — a
    // worker without the helper has nothing to offer and says no by absence.
    // ---------------------------------------------------------------------
    Menu {
        id: mediaContextMenu
        objectName: "mediaContextMenu"

        MenuItem {
            objectName: "mediaAddNdiAction"
            text: "Add NDI source\u2026"
            visible: worker.liveCapable
            enabled: worker.liveCapable
            onTriggered: {
                // A short first scan, then the dialog's timer keeps looking.
                worker.refreshLiveSources(1500, true)
                ndiSourceDialog.open()
            }
        }
        MenuItem {
            objectName: "mediaContextImportAction"
            text: "Import\u2026"
            onTriggered: mediaDialog.open()
        }
    }

    Dialog {
        id: ndiSourceDialog
        objectName: "ndiSourceDialog"
        title: "Add NDI source"
        modal: true
        width: 460
        height: 360

        contentItem: Column {
            spacing: 8
            Text {
                width: parent.width
                color: "#b0b0b0"
                font.pixelSize: 12
                wrapMode: Text.WordWrap
                // One message per settled state. A rescan only says
                // "Scanning..." when there is nothing else to show, so a
                // refresh cannot overwrite the instruction with a transient
                // line and then back again.
                text: !worker.liveCapable
                      ? "NDI is not available in this worker."
                      : (worker.liveSources.length > 0
                         ? "Choose a camera. It becomes a Media Bin asset; set it as a slide background in Edit."
                         : (worker.liveScanning
                            ? "Scanning the network\u2026"
                            : "No cameras yet \u2014 they appear here as they start broadcasting."))
            }
            ListView {
                objectName: "ndiSourceList"
                width: parent.width
                height: 240
                clip: true
                model: worker.liveSources
                delegate: ItemDelegate {
                    width: ListView.view.width
                    text: modelData.name
                    onClicked: {
                        if (docs.addCameraAsset(modelData.name, modelData.name)) {
                            ndiSourceDialog.close()
                        }
                    }
                }
            }
            Button {
                objectName: "ndiRescanButton"
                text: worker.liveScanning ? "Scanning\u2026" : "Rescan"
                enabled: worker.liveCapable && !worker.liveScanning
                onClicked: worker.refreshLiveSources(2500, true)
            }
            Button {
                text: "Close"
                onClicked: ndiSourceDialog.close()
            }
        }
    }

    // The picker stays live while it is open: a camera that starts
    // broadcasting after the dialog opened still appears without reopening
    // it. The worker's helper keeps a finder, so the zero-wait scan returns
    // what it already knows and costs nothing.
    Timer {
        id: ndiRescanTimer
        // Our share of the lag: poll sooner, but cheaply. The wait itself is
        // the NDI runtime's finder, which reports a new source as soon as it
        // sees one, so there is no fixed countdown to offer.
        interval: 1000
        repeat: true
        running: ndiSourceDialog.opened && worker.liveCapable
        onTriggered: worker.refreshLiveSources(0)
    }

    // Right-click anywhere on the bin. A handler, not a covering MouseArea:
    // it takes only the secondary button and leaves every left click, hover
    // and wheel event to the controls underneath.
    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: mediaContextMenu.popup()
    }
}
