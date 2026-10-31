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
// and import kept separate from every activation — nothing here sends
// anything to the worker, and the two controls that change the document stay
// behind editingEnabled. A file the product cannot present is still listed
// and says so: hiding the row would leave the operator wondering where the
// import went.
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
    // What a row adds to its name. The kind chip already says what the file
    // IS, so this is only the part the operator acts on — and a picture needs
    // nothing: it is the kind that works. Each notice is kept short enough to
    // read whole in the row's notice column (the suite checks that), because a
    // warning that elides into "background on…" explains nothing.
    function noticeFor(kind) {
        if (kind === "audio") return "nothing plays it from the bin yet"
        if (kind === "video") return "background only, not a slide picture"
        if (kind === "other") return "nothing presents this file type"
        return ""
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
                             + (mediaBin.noticeFor(mediaBin.selected.kind) === ""
                                ? "" : " \u00b7 " + mediaBin.noticeFor(mediaBin.selected.kind)))
                          : "Pick a media item to inspect it"
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
                                         + (mediaBin.noticeFor(modelData.kind) === ""
                                            ? "" : ", " + mediaBin.noticeFor(modelData.kind))

                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: 1
                            color: mediaRow.rowAssetId === mediaBin.selectedAssetId
                                ? "#1685f8" : (rowMouse.containsMouse ? "#4a4a4a" : "transparent")
                        }

                        // Name, kind, and — when the row is wide enough for
                        // all three — why this file cannot be used. The name
                        // takes whatever the two fixed columns leave, so a
                        // long file name elides in the middle instead of
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
                                         && mediaBin.noticeFor(modelData.kind) !== ""
                                elide: Text.ElideRight
                                font.pixelSize: 11
                                color: "#c8a24a"
                                text: mediaBin.noticeFor(modelData.kind)
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
