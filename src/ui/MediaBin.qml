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
        picture: "Pictures", video: "Video", audio: "Audio", other: "Other"
    })
    // One row per kind the library actually holds, each with its count, and
    // "All media" first: it is the view the operator widens back out to, and
    // the only one an empty library has. These are the collections the bin can
    // honour — every one is a filter over rows whose type the product already
    // knows — and a kind with no assets is absent rather than a dead click.
    readonly property var collections: {
        const counts = ({picture: 0, video: 0, audio: 0, other: 0})
        for (const asset of docs.assets) {
            if (counts[asset.kind] === undefined) counts[asset.kind] = 0
            counts[asset.kind]++
        }
        const list = [{kind: "", label: "All media", count: docs.assets.length}]
        for (const kind of ["picture", "video", "audio", "other"]) {
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
        if (row.kind === "other") return "nothing presents this file type"
        // An audio cue, off air: what is standing between this row and sound.
        if (!worker.connected) return "no output connected \u2014 nothing can play"
        if (mediaBin.isRefused(row))
            return "cannot play audio: " + mediaBin.fileName(row.sourcePath)
        if (worker.standaloneOnAir) return "replaces the track now on air"
        return "press Play to start it on the production audio track"
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

            Row {
                id: mediaActions
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                spacing: 6

                // The bin's runtime cue (UI.4b), and the only one this product
                // has: the worker's independent production audio track, which
                // slide changes neither restart nor stop. The kinds with no cue
                // of their own are absent rather than greyed out — a video
                // reaches the audience as a slide's background and a picture as
                // a slide's content, both authored in Edit, and a disabled Play
                // would read as a fault with the output when the truth is that
                // this file has no runtime to fire.
                //
                // Nothing here edits the document, so nothing here waits for
                // Edit: this is a presenter's control. And what the row shows
                // afterwards is the worker's, never the click's — the track
                // reads ON AIR when the status block says it is live, and a
                // refused probe reads "cannot play" with the reason beside it.
                PanelButton {
                    objectName: "mediaActivateButton"
                    visible: mediaBin.selected.kind === "audio"
                    enabled: mediaBin.canActivate
                    text: mediaBin.selectedLive ? "Stop" : "Play"
                    Accessible.name: text + (mediaBin.canActivate
                                             ? "" : " \u2014 no output connected")
                    onClicked: worker.selectStandaloneTrack(
                        mediaBin.selectedLive ? "" : mediaBin.selected.sourcePath)
                }
                PanelButton {
                    objectName: "mediaImportButton"
                    text: "Import\u2026"
                    onClicked: mediaDialog.open()
                }
                PanelButton {
                    objectName: "mediaRemoveButton"
                    visible: mediaBin.editingEnabled
                    text: "Remove"
                    enabled: mediaBin.selectedAssetId !== ""
                    onClicked: {
                        docs.removeAsset(mediaBin.selectedAssetId)
                        mediaBin.selectedAssetId = ""
                    }
                }
                // Task 6.17b2's placement, still Edit-only and now honest
                // about its range: a picture is the only kind this build
                // composites onto a slide, so the button offers that kind and
                // no other. The model's own refusal still stands behind it
                // (Documents::addItem turns a video down by name), so this is
                // the surface matching the rule rather than a second copy of
                // the rule — and a name nothing claims is not offered either,
                // even though the model would take it and the renderer would
                // then have nothing to draw.
                PanelButton {
                    objectName: "placeAssetButton"
                    visible: mediaBin.editingEnabled
                    text: "Add to slide"
                    enabled: mediaBin.selected.kind === "picture"
                    onClicked: docs.placeAssetOnSlide(mediaBin.selectedAssetId)
                }
            }

            Rectangle {
                anchors.top: mediaActions.bottom
                anchors.topMargin: 8
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                color: "#242424"
                border.color: "#4a4a4a"

                ListView {
                    id: mediaList
                    objectName: "mediaBinList"
                    anchors.fill: parent
                    anchors.margins: 1
                    clip: true
                    model: mediaBin.rows
                    ScrollBar.vertical: ScrollBar {}

                    delegate: Item {
                        id: mediaRow
                        objectName: "mediaBinRow"

                        required property var modelData
                        width: mediaList.width
                        height: 28
                        readonly property string rowAssetId: modelData.id
                        readonly property string rowKind: modelData.kind
                        Accessible.name: modelData.name + ", "
                                         + mediaBin.kindLabels[modelData.kind]
                                         + (mediaBin.noticeFor(modelData) === ""
                                            ? "" : ", " + mediaBin.noticeFor(modelData))

                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: 1
                            color: mediaRow.rowAssetId === mediaBin.selectedAssetId
                                ? "#1685f8" : (rowMouse.containsMouse ? "#4a4a4a" : "transparent")
                        }

                        // Name, kind, and — when the row is wide enough for
                        // all three — the one thing worth knowing about this
                        // file: that it is on air, or why it cannot be used.
                        // The name takes whatever the two fixed columns leave,
                        // so a long file name elides in the middle instead of
                        // pushing the kind off the row; and at bin widths
                        // where the notice would eat the name it steps aside
                        // and the info column carries the explanation alone.
                        Row {
                            id: rowParts
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.leftMargin: 8
                            anchors.rightMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 10
                            readonly property real kindWidth: 78
                            readonly property real noticeWidth: mediaRow.width >= 470 ? 200 : 0

                            Text {
                                objectName: "mediaRowName"
                                width: rowParts.width - rowParts.kindWidth
                                       - rowParts.noticeWidth
                                       - rowParts.spacing * (rowParts.noticeWidth > 0 ? 2 : 1)
                                elide: Text.ElideMiddle
                                font.pixelSize: 13
                                color: mediaRow.rowAssetId === mediaBin.selectedAssetId
                                    ? "#ffffff" : "#eeeeee"
                                text: modelData.name
                            }
                            Text {
                                objectName: "mediaRowKind"
                                width: rowParts.kindWidth
                                elide: Text.ElideRight
                                font.pixelSize: 11
                                color: mediaRow.rowAssetId === mediaBin.selectedAssetId
                                    ? "#d9ebff" : "#8fb7dd"
                                text: mediaBin.kindLabels[modelData.kind]
                            }
                            Text {
                                objectName: "mediaRowNotice"
                                width: rowParts.noticeWidth
                                visible: rowParts.noticeWidth > 0
                                         && mediaBin.noticeFor(modelData) !== ""
                                elide: Text.ElideRight
                                font.pixelSize: 11
                                color: mediaBin.noticeColor(modelData,
                                                           mediaRow.rowAssetId
                                                           === mediaBin.selectedAssetId)
                                text: mediaBin.noticeFor(modelData)
                            }
                        }
                        MouseArea {
                            id: rowMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            acceptedButtons: Qt.LeftButton
                            // Selecting a media row inspects it. It does not
                            // present, does not place, and does not touch the
                            // slide under edit — activation is UI.4b's, and
                            // this is the boundary the acceptance names.
                            onClicked: mediaBin.selectedAssetId = mediaRow.rowAssetId
                        }
                    }

                    Text {
                        anchors.centerIn: parent
                        visible: mediaList.count === 0
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
}
