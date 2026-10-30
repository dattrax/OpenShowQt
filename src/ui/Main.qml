import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material

/*
 * The operator workspace (M4 task 4.1): the reference's shell — a 40px
 * top bar over a row of left panel / center / right panel — with the
 * lyric flow built through M3 living inside it. The side panels are the
 * reference's resizable ones (SidePanel.qml + WorkspacePanels' measured
 * snap rules); the centre carries the slide list and the editor, and
 * the right panel is always the preview with the output tools below it,
 * like the reference's Preview + ShowTools.
 *
 * Theme tokens are the reference palette (docs/reference.md §3).
 *
 * Since task 6.19b3 the window's whole key table lives in ShortcutMap.qml;
 * what remains here is execution — the functions and buttons that perform
 * what a key asked, and the focus facts only the window can speak for.
 */
Window {
    id: root
    width: uiSize.width
    height: uiSize.height
    minimumWidth: 400   // the reference's window minimum
    minimumHeight: 400
    visible: true
    color: "#191923"  // --primary-darker, the canvas behind everything
    title: "OpenShowQt " + applicationVersion
    // Task 4.5: the F11/settings fullscreen is a persisted app setting,
    // so a projector setup survives restarts.
    visibility: appSettings.fullscreen ? Window.FullScreen : Window.Windowed

    Material.theme: Material.Dark
    Material.accent: "#f0008c"  // --secondary

    // Task 1.2: closing the UI shuts the worker down before the window
    // actually goes away.
    onClosing: function(close) {
        worker.shutdownNow()
    }

    // The workspace page under the top bar (4.3): "show" is the running
    // order and lyric editor, "edit" the text-box canvas. The remaining
    // reference pages arrive later and their buttons stay disabled.
    property string page: "show"

    // True while a text field holds the focus — the plain-arrow nudges
    // must not steal keystrokes from someone typing.
    readonly property bool typingIntoField: activeFocusItem !== null
        && (activeFocusItem instanceof TextField || activeFocusItem instanceof TextArea
            || activeFocusItem instanceof TextInput)

    // Task 3.3: once a slide has been presented from the panel, navigating
    // follows it onto the output — that is the operator's expectation
    // mid-service. Any Present click (re)arms it.
    property bool following: false

    // Panel building blocks (task 4.4): Material's default TextField is
    // too tall for a 250px column once the background is replaced (the
    // placeholder and text split vertically), and its Button is too wide
    // for three in a row. Since task 6.19b4 they are module files
    // (PanelField.qml, PanelButton.qml) carrying the workspace palette and
    // metrics, so the panels that got their own files style themselves
    // without borrowing the window's private declarations.

    function presentCurrentSlide() {
        const json = docs.currentSceneJson()
        if (json !== "") {
            worker.presentSceneJson(json)
        }
    }

    // A focused button or combo eats Space and the arrow keys themselves;
    // the presenter gates stand down for it (task 4.5, AbstractButton is
    // both in the reference's rule). The window has to be the one to say
    // this: activeFocusItem is the window's, and a panel's own
    // activeFocusItem would answer a different question.
    readonly property bool squattedFocus: activeFocusItem !== null
        && (activeFocusItem instanceof AbstractButton || activeFocusItem instanceof ComboBox)

    // The key actions themselves (task 6.19b3) live in ShortcutMap.qml —
    // every sequence, its gate, and the intent it carries, so R17's
    // one-owner-per-key rule reads in one file. What stays here is
    // execution: the map emits an intent, and these functions (and the
    // buttons below) are the ones that perform it against the canvas, the
    // controller and the worker.

    // Task 4.3: the reference's nudge rule (EditTools.svelte:314-326).
    // Since 6.17a2 "the selected box" is the session's selection: the
    // canvas mirrors its rectangle, and arrows, drag, fields and inspector
    // all move the box that wears the handles.
    function nudgeBox(dx, dy) {
        if (docs.currentSlide < 0 || docs.slides.length === 0) {
            return
        }
        docs.setTextBoxGeometry(editCanvas.boxX + dx, editCanvas.boxY + dy,
                                editCanvas.boxW, editCanvas.boxH)
    }
    // Task 6.17a2: step the selection through the slide's stack, wrapping.
    function stepBox(delta) {
        const count = editCanvas.boxes.length
        if (count === 0) {
            return
        }
        docs.selectTextBox((docs.currentBox + delta + count) % count)
    }
    // Task 4.5: the presenter's steps walk the same navigation a Present
    // click armed, so a service runs from the keyboard alone.
    function presenterStep(delta) {
        if (docs.slides.length === 0) {
            return
        }
        docs.stepSlide(delta)
        root.presentCurrentSlide()
    }
    function presenterJump(index) {
        if (index >= 0 && index < docs.slides.length) {
            docs.selectSlide(index)
            root.presentCurrentSlide()
        }
    }

    // Task 6.19b3: the whole key table. The facts only the window and its
    // panels can speak for arrive as the properties below — which page is
    // up, what holds the focus (typing, a squatted button, the drawer, the
    // canvas's handles) and the on-air state the clear keys gate on — and
    // every activated key comes back as a signal handled here, beside the
    // same buttons that perform it. No key changed owner, sequence or gate
    // on the way out; two enabled owners for one key make Qt drop the key
    // entirely, which is why the gates moved WITH the shortcuts rather
    // than staying behind.
    ShortcutMap {
        page: root.page
        typingIntoField: root.typingIntoField
        squattedFocus: root.squattedFocus
        drawerFocused: libraryDrawer.containsFocus
        boxSelected: editCanvas.selected
        currentBox: docs.currentBox
        slideCount: docs.slides.length
        wordsOnAir: worker.wordsOnAir
        backgroundOnAir: worker.backgroundOnAir
        canRestore: worker.canRestore
        numberKeysPresent: appSettings.numberKeysPresent

        // Escape clears all — and restores when the output is already
        // clear, matching the button's Clear-all/Restore toggle (3.4).
        onOutputCancelRequested: worker.canRestore ? worker.restoreOutput()
                                                   : worker.clearAll()
        onDeselectBoxRequested: editCanvas.selected = false
        onClearBackgroundRequested: worker.clearBackground()
        onClearWordsRequested: worker.clearSlide()
        onDuplicateSlideRequested: docs.duplicateCurrentSlide()
        onToggleDrawerRequested: panels.toggleDrawer()
        onFocusDrawerSearchRequested: libraryDrawer.focusSearch()
        onPageSelected: name => root.page = name
        onBoxNudgeRequested: (dx, dy) => root.nudgeBox(dx, dy)
        onBoxStepRequested: delta => root.stepBox(delta)
        onPresenterStepRequested: delta => root.presenterStep(delta)
        onPresenterJumpRequested: index => root.presenterJump(index)
        onClearAllRequested: worker.clearAll()
        onUndoRequested: docs.undo()
        onRedoRequested: docs.redo()
        onSaveRequested: docs.save()
        onCopyRequested: docs.copyCurrentSlide()
        onCutRequested: docs.cutCurrentSlide()
        onPasteRequested: docs.pasteSlide()
        onFullscreenToggled: appSettings.fullscreen = !appSettings.fullscreen
    }

    TopBar {
        id: topBar
        page: root.page
        onPageSelected: name => root.page = name
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
    }

    /* ------------------------------------------------------------------
     * Left panel — the show page of the reference: playlist chips over
     * the running order. Task 6.19b5: the dock is PlaylistPanel.qml —
     * its chips, rows, reorder handles and drop target all address
     * `docs` and the rows are its projections; what the window owns is
     * where the dock sits and the id the centre pages anchor to.
     * ------------------------------------------------------------------ */
    PlaylistPanel {
        id: leftDock
        anchors.left: parent.left
        anchors.top: topBar.bottom
        anchors.bottom: libraryDrawer.top
    }

    /* ------------------------------------------------------------------
     * Right panel — always the preview, with the show's output tools
     * below it (the reference's Preview + ShowTools arrangement). Task
     * 6.19b4: the dock itself is OutputPanel.qml — it mirrors `worker`
     * status and drags through `panels` on its own; what the window still
     * owns is where it sits, between the top bar and the drawer, and the
     * id the two centre pages anchor their right edges to.
     * ------------------------------------------------------------------ */
    OutputPanel {
        id: rightDock
        anchors.right: parent.right
        anchors.top: topBar.bottom
        anchors.bottom: libraryDrawer.top
    }

    /* ------------------------------------------------------------------
     * Center — two pages (4.3) on the reference's show/edit split. The
     * show page: the current song's slide list and the lyric editor.
     * Everything here drives DocumentController ("docs"); presenting
     * compiles the selected slide and hands the JSON to the worker,
     * which presents on the acknowledgement.
     * ------------------------------------------------------------------ */
    Item {
        id: showPage
        visible: root.page === "show"
        anchors.left: leftDock.right
        anchors.right: rightDock.left
        anchors.top: topBar.bottom
        anchors.bottom: libraryDrawer.top
        anchors.margins: 12

        Row {
            id: slideActions
            anchors.top: parent.top
            anchors.left: parent.left
            spacing: 8

            Button {
                text: "Add slide"
                enabled: docs.currentSongId !== ""
                onClicked: docs.addSlide()
            }

            Button {
                text: "Remove slide"
                enabled: docs.slides.length > 0
                onClicked: docs.removeCurrentSlide()
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                color: "#8f93a5"
                font.pixelSize: 12
                text: docs.slides.length > 0
                    ? "slide " + (docs.currentSlide + 1) + " of " + docs.slides.length
                    : "no slides"
            }
        }

        // The slide list's pictures (task 6.16b, review R16): the worker
        // renders the visible band with the production compositor, so a row
        // shows the slide as the audience would see it — same fonts, same
        // wrap, same background — instead of a line of Qt-laid-out text.
        //
        // The band is the current slide ±5, which is the rows on screen (the
        // box fits about six at 26 px) plus a scroll's worth either way, and
        // it is inside the 32-slide cap the protocol declares. Asking per
        // visible row would send one batch per pixel of scroll; asking for a
        // band moves the list in steps, and the client coalesces a band it
        // already has. A slide the worker has not drawn yet keeps its text
        // line: the fallback is the row's own, so a list is never blank while
        // pictures are on their way.
        function requestSlideBand() {
            if (!worker.thumbnailSupported) {
                return
            }
            const slides = docs.slides
            if (slides.length === 0) {
                worker.clearThumbnails()
                return
            }
            const band = []
            const first = Math.max(0, docs.currentSlide - 5)
            const last = Math.min(slides.length - 1, docs.currentSlide + 5)
            for (let i = first; i <= last; ++i) {
                band.push({key: slides[i].id,
                           scene: docs.sceneJsonForSlide(slides[i].id)})
            }
            worker.requestThumbnails(band, 160, 90)
        }

        // Navigating, editing and editing the document (a slide added, a song
        // switched) all change what the list wants a picture of. The client
        // re-issues across a preview restart and a worker that dropped the
        // batch on its own, so the list does not have to watch the socket too.
        Connections {
            target: docs
            function onNavigationChanged() { showPage.requestSlideBand() }
            function onDocumentsChanged() { showPage.requestSlideBand() }
            function onSlideTextChanged() { showPage.requestSlideBand() }
        }
        // The capability arrives with the handshake, which is usually after
        // this page was built. ONE ask follows the first moment the worker can
        // draw the list: previewChanged fires with every preview frame, and a
        // handler that asked each time would recompile eleven scenes fifteen
        // times a second for a list that already has them.
        property bool bandAskedFor: false
        Connections {
            target: worker
            function onPreviewChanged() {
                if (worker.thumbnailSupported && !showPage.bandAskedFor) {
                    showPage.bandAskedFor = true
                    showPage.requestSlideBand()
                }
            }
        }
        Component.onCompleted: Qt.callLater(function() { showPage.requestSlideBand() })

        Rectangle {
            id: slideListBox
            anchors.top: slideActions.bottom
            anchors.topMargin: 8
            anchors.left: parent.left
            anchors.right: parent.right
            height: 150
            color: "#12121c"
            border.color: "#2f3542"

            ListView {
                id: slideList
                anchors.fill: parent
                anchors.margins: 1
                clip: true
                model: docs.slides

                delegate: ItemDelegate {
                    id: control
                    required property var modelData
                    width: slideList.width
                    height: 26
                    text: (modelData.index + 1) + ".  " + modelData.preview
                    highlighted: modelData.index === docs.currentSlide
                    contentItem: Row {
                        spacing: 6
                        // The worker's own picture of this slide (task 6.16b).
                        // It is an addition, not a replacement: the text line
                        // stays, so a row is legible before its picture
                        // arrives, on a worker that cannot draw the list, and
                        // to a screen reader either way.
                        PreviewView {
                            id: rowPicture
                            visible: worker.thumbnailSupported
                            width: 46
                            height: 26
                            anchors.verticalCenter: parent.verticalCenter
                            controller: worker
                            target: PreviewView.Thumbnail
                            key: modelData.id
                        }
                        Text {
                            // The picture's 46 px only when there is one: a
                            // positioner lays out nothing for an invisible
                            // child, so narrowing the line for a picture that
                            // is not there would elide it for no reason.
                            width: slideList.width
                                   - (rowPicture.visible ? rowPicture.width : 0) - 40
                            text: control.text
                            color: control.highlighted ? "#ffffff" : "#f0f0ff"
                            font.pixelSize: 13
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                        }
                    }
                    background: Rectangle {
                        color: control.highlighted ? "#f0008c"
                             : (control.hovered ? "#2f3542" : "transparent")
                    }
                    onClicked: {
                        docs.selectSlide(modelData.index)
                        if (root.following) {
                            root.presentCurrentSlide()
                        }
                    }
                }
            }
        }

        Row {
            id: historyRow
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            spacing: 8

            Button {
                text: "Undo"
                enabled: docs.canUndo
                onClicked: docs.undo()
            }

            Button {
                text: "Redo"
                enabled: docs.canRedo
                onClicked: docs.redo()
            }

            Button {
                text: "Save"
                enabled: docs.dirty
                onClicked: docs.save()
            }

            Button {
                text: "Load"
                onClicked: docs.load()
            }
        }

        Row {
            id: navRow
            anchors.bottom: historyRow.top
            anchors.bottomMargin: 10
            anchors.left: parent.left
            spacing: 8

            Button {
                text: "\u25c0 Prev"
                enabled: docs.slides.length > 0
                onClicked: {
                    docs.stepSlide(-1)
                    if (root.following) {
                        root.presentCurrentSlide()
                    }
                }
            }

            Button {
                text: "Next \u25b6"
                enabled: docs.slides.length > 0
                onClicked: {
                    docs.stepSlide(1)
                    if (root.following) {
                        root.presentCurrentSlide()
                    }
                }
            }

            Button {
                text: "Present"
                enabled: docs.slides.length > 0 && worker.outputLive
                onClicked: {
                    root.following = true
                    root.presentCurrentSlide()
                }
            }

            CheckBox {
                text: "follow"
                checked: root.following
                onToggled: root.following = checked
            }
        }

        // The editor writes into the debounced controller slot; the
        // binding is re-established (not just assigned) when navigation,
        // undo or a load changes the slide under the operator, so future
        // changes keep flowing without breaking typing again. Since
        // 6.17b2 the selection may be a picture, which has no words to
        // edit — offering an empty field for one invites a typing burst
        // the model must refuse, so the dock shows the picture's asset
        // instead, and the field returns the moment a text box is
        // selected again.
        Column {
            id: pictureInfo
            objectName: "pictureInfo"
            anchors.top: slideListBox.bottom
            anchors.topMargin: 10
            anchors.bottom: navRow.top
            anchors.bottomMargin: 10
            anchors.left: parent.left
            anchors.right: parent.right
            visible: docs.currentBoxKind === "image"
            spacing: 6

            Text {
                color: "#8f93a5"
                font.pixelSize: 10
                font.bold: true
                text: "PICTURE"
            }
            Text {
                objectName: "pictureAssetName"
                width: parent.width
                elide: Text.ElideMiddle
                color: "#f0f0ff"
                font.pixelSize: 14
                text: docs.currentBoxAsset.name || "this picture has lost its asset"
            }
            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                color: "#8f93a5"
                font.pixelSize: 11
                text: docs.currentBoxAsset.sourcePath || " "
            }
            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                color: "#6a6f80"
                font.pixelSize: 11
                text: "drag on the canvas to place it \u00b7 Back/Forward reorder "
                      + "\u00b7 Delete takes it off the slide"
            }
        }

        TextArea {
            id: lyricText
            visible: docs.currentBoxKind !== "image"
            anchors.top: slideListBox.bottom
            anchors.topMargin: 10
            anchors.bottom: navRow.top
            anchors.bottomMargin: 10
            anchors.left: parent.left
            anchors.right: parent.right
            text: docs.currentSlideText
            onTextChanged: docs.setSlideText(text)
            selectByMouse: true
            wrapMode: TextArea.Wrap
            color: "#f0f0ff"
            background: Rectangle {
                color: "#12121c"
                border.color: "#2f3542"
            }

            Connections {
                target: docs
                function onSlideTextChanged() {
                    lyricText.text = Qt.binding(function () { return docs.currentSlideText })
                    lyricText.cursorPosition = 0
                }
            }
        }
    }

    /* ------------------------------------------------------------------
     * The edit page (task 4.3, stack since 6.17a2): the current slide's
     * boxes on a 16:9 stage — click selects the topmost box, drag moves
     * it, corners resize it, Tab walks the stack, arrows nudge — with
     * exact X/Y/W/H fields and New/Delete/Back/Forward under it as the
     * deterministic path. One undo entry per drag, per nudge, per field
     * commit, and per stack command.
     * ------------------------------------------------------------------ */
    Item {
        id: editPage
        visible: root.page === "edit"
        anchors.left: leftDock.right
        anchors.right: rightDock.left
        anchors.top: topBar.bottom
        anchors.bottom: libraryDrawer.top
        anchors.margins: 12

        EditCanvas {
            id: editCanvas
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: stylePanel.left
            anchors.rightMargin: 12
            anchors.bottom: boxFields.top
            anchors.bottomMargin: 12
        }

        /*
         * Asking the worker for its picture of the slide under edit (task
         * 6.16a, review R16). Debounced: one ask costs a scene.prepare and an
         * off-air draw, so a keystroke at a time would be renders nobody has
         * looked at yet. The triggers below are everything that can change
         * what the canvas should show — text, style, geometry (a drag
         * commits on release, which is a document change), navigation and
         * page entry — and the payload is compiled at the tick, so the
         * render is of the slide as it ended up rather than as it was when
         * typing started.
         */
        Timer {
            id: selectionAsk
            interval: 250
            repeat: false
            onTriggered: {
                if (!editPage.visible || !editCanvas.hasSlide) {
                    worker.clearSelectionPreview()
                    return
                }
                worker.requestSelectionPreview(
                    docs.sceneJsonForSlide(docs.slides[docs.currentSlide].id))
            }
        }
        function askForWorkerPicture() {
            selectionAsk.restart()
        }
        Connections {
            target: docs
            function onDocumentsChanged() {
                if (editPage.visible) {
                    editPage.askForWorkerPicture()
                }
            }
            function onSlideTextChanged() {
                if (editPage.visible) {
                    editPage.askForWorkerPicture()
                }
            }
            function onNavigationChanged() {
                if (editPage.visible) {
                    editPage.askForWorkerPicture()
                }
            }
        }
        // Leaving the page stands the target down: the worker stops drawing
        // (and holding a scene for) a slide nobody is looking at, and the
        // stream goes back to mirroring the audience alone.
        onVisibleChanged: {
            if (editPage.visible) {
                editPage.askForWorkerPicture()
            } else {
                selectionAsk.stop()
                worker.clearSelectionPreview()
            }
        }

        Row {
            id: boxFields
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            spacing: 8

            // The fields show the live box; the one that loses focus
            // commits all four. A value the domain refuses (or that is
            // not a number at all) snaps every field back.
            function commit() {
                const x = parseFloat(fieldX.text)
                const y = parseFloat(fieldY.text)
                const w = parseFloat(fieldW.text)
                const h = parseFloat(fieldH.text)
                // The exact-number path onto the SELECTED box — the same
                // op the drag commits, so one entry lands either way.
                if (!Number.isFinite(x) || !Number.isFinite(y)
                        || !(w >= 1) || !(h >= 1)
                        || !docs.setTextBoxGeometry(x, y, w, h)) {
                    boxFields.sync()
                }
            }
            function sync() {
                fieldX.text = Qt.binding(function () { return editCanvas.boxX.toFixed(0) })
                fieldY.text = Qt.binding(function () { return editCanvas.boxY.toFixed(0) })
                fieldW.text = Qt.binding(function () { return editCanvas.boxW.toFixed(0) })
                fieldH.text = Qt.binding(function () { return editCanvas.boxH.toFixed(0) })
            }
            Component.onCompleted: sync()
            Connections {
                target: docs
                function onDocumentsChanged() { boxFields.sync() }
                function onNavigationChanged() { boxFields.sync() }
            }
            Connections {
                target: editPage
                function onVisibleChanged() { if (editPage.visible) boxFields.sync() }
            }

            Column {
                spacing: 2
                Text {
                    color: "#8f93a5"
                    font.pixelSize: 10
                    text: "X"
                }
                TextField {
                    id: fieldX
                    objectName: "geometryField"
                    width: 72
                    color: "#f0f0ff"
                    font.pixelSize: 13
                    onEditingFinished: boxFields.commit()
                    background: Rectangle {
                        color: "#12121c"
                        border.color: "#2f3542"
                    }
                }
            }

            Column {
                spacing: 2
                Text {
                    color: "#8f93a5"
                    font.pixelSize: 10
                    text: "Y"
                }
                TextField {
                    id: fieldY
                    objectName: "geometryField"
                    width: 72
                    color: "#f0f0ff"
                    font.pixelSize: 13
                    onEditingFinished: boxFields.commit()
                    background: Rectangle {
                        color: "#12121c"
                        border.color: "#2f3542"
                    }
                }
            }

            Column {
                spacing: 2
                Text {
                    color: "#8f93a5"
                    font.pixelSize: 10
                    text: "W"
                }
                TextField {
                    id: fieldW
                    objectName: "geometryField"
                    width: 72
                    color: "#f0f0ff"
                    font.pixelSize: 13
                    onEditingFinished: boxFields.commit()
                    background: Rectangle {
                        color: "#12121c"
                        border.color: "#2f3542"
                    }
                }
            }

            Column {
                spacing: 2
                Text {
                    color: "#8f93a5"
                    font.pixelSize: 10
                    text: "H"
                }
                TextField {
                    id: fieldH
                    objectName: "geometryField"
                    width: 72
                    color: "#f0f0ff"
                    font.pixelSize: 13
                    onEditingFinished: boxFields.commit()
                    background: Rectangle {
                        color: "#12121c"
                        border.color: "#2f3542"
                    }
                }
            }

            // The stack's commands (task 6.17a2). One undo step each, and
            // their enabled state reads the SELECTION: Delete takes the
            // selected box away, Back/Forward re-order it in the stack the
            // worker paints — what moves forward lands on top of the
            // audience's picture, exactly where the outlines here say it
            // will. "New" stays live at the ceiling and lets the status
            // line name the limit; the refusal adds no undo step.
            Column {
                spacing: 2
                Text {
                    color: "#8f93a5"
                    font.pixelSize: 10
                    text: "STACK"
                }
                Row {
                    spacing: 4
                    PanelButton {
                        objectName: "newBoxButton"
                        text: "New box"
                        onClicked: docs.addTextBox()
                    }
                    PanelButton {
                        objectName: "deleteBoxButton"
                        text: "Delete"
                        enabled: docs.currentBox >= 0
                        onClicked: docs.removeCurrentTextBox()
                    }
                    PanelButton {
                        objectName: "boxBackButton"
                        text: "Back"
                        enabled: docs.currentBox > 0
                        onClicked: docs.moveCurrentTextBox(docs.currentBox - 1)
                    }
                    PanelButton {
                        objectName: "boxForwardButton"
                        text: "Forward"
                        enabled: docs.currentBox >= 0
                                 && docs.currentBox < docs.currentSlideBoxes.length - 1
                        onClicked: docs.moveCurrentTextBox(docs.currentBox + 1)
                    }
                }
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                color: "#6a6f80"
                font.pixelSize: 11
                text: "drag a box · corners resize · Tab next box · arrows nudge 1px, Ctrl 10px"
            }
        }

        /*
         * The style panel (task 4.4): one style object is under edit —
         * the one the current slide resolves through, override or show
         * default. Editing it changes every slide that follows it, which
         * is the reuse the reference gives its themes; New forks the
         * look for this slide alone, Show default hands it to every
         * un-overridden slide, and Follow show (the empty id) gives the
         * override back. Fields commit on focus-out and snap back on a
         * refusal, exactly like the box fields beside them.
         */
        Column {
            id: stylePanel
            objectName: "stylePanel"
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            width: 250
            spacing: 10

            // The style the fields edit right now ("" = none anywhere).
            function targetId() {
                return docs.currentStyle.id || ""
            }
            // Combo row 0 is "follow show" (the empty id); the rest name
            // the styles view's rows in order.
            function styleIdAt(index) {
                if (index <= 0) {
                    return ""
                }
                var i = index - 1
                return i < docs.styles.length ? docs.styles[i].id : ""
            }
            // Name, colours, font and typography commit as one edit.
            // A slide following nothing creates its style on first edit.
            function commit(newAlign) {
                const size = parseFloat(fieldStyleSize.text)
                const spacing = parseFloat(fieldLineSpacing.text)
                const align = newAlign !== undefined ? newAlign
                                                     : (docs.currentStyle.align || "center")
                const name = fieldStyleName.text.trim() || "Style"
                const colour = fieldTextColor.text.trim()
                const back = fieldBackColour.text.trim()
                // "" is a real answer here, not a missing value: it is how
                // the operator says the background stays flat (6.17c3).
                const gradient = fieldBackGradient.text.trim()
                const family = fieldFontFamily.text.trim()
                const spacingValue = Number.isFinite(spacing) ? spacing : 1.25
                const sizeValue = Number.isFinite(size) && size >= 0 ? size : 0
                if (targetId() === "") {
                    if (!docs.createStyleFromCurrent(name)) {
                        stylePanel.sync()
                        return
                    }
                    // The creation took the built-in look; carry the
                    // operator's values onto it, undoing the creation too
                    // if the domain refuses them.
                    if (!docs.editStyle(targetId(), name, colour, back, gradient, family,
                                        sizeValue, align, spacingValue)) {
                        docs.undo()
                        stylePanel.sync()
                    }
                    return
                }
                if (!docs.editStyle(targetId(), name, colour, back, gradient, family,
                                    sizeValue, align, spacingValue)) {
                    stylePanel.sync()
                }
            }
            function sync() {
                fieldStyleName.text = Qt.binding(function () {
                    return docs.currentStyle.name || ""
                })
                fieldTextColor.text = Qt.binding(function () {
                    return docs.currentStyle.textColor || "#ffffff"
                })
                fieldBackColour.text = Qt.binding(function () {
                    return docs.currentStyle.backgroundColor || "#000000"
                })
                // Flat shows empty, not a colour: the field IS the "" that
                // commit passes back (6.17c3).
                fieldBackGradient.text = Qt.binding(function () {
                    return docs.currentStyle.backgroundGradient || ""
                })
                fieldFontFamily.text = Qt.binding(function () {
                    return docs.currentStyle.fontFamily || ""
                })
                fieldStyleSize.text = Qt.binding(function () {
                    const size = docs.currentStyle.fontSize
                    return size === undefined ? "0" : String(size)
                })
                fieldLineSpacing.text = Qt.binding(function () {
                    const spacing = docs.currentStyle.lineSpacing
                    return spacing === undefined ? "1.25" : String(spacing)
                })
                // The combo names the followed default as its first row;
                // an override selects its own style instead.
                var index = 0
                if (docs.currentStyleIsOverride) {
                    for (var i = 0; i < docs.styles.length; ++i) {
                        if (docs.styles[i].id === stylePanel.targetId()) {
                            index = i + 1
                            break
                        }
                    }
                }
                styleCombo.currentIndex = index
                var background = ""
                if (docs.currentSlide >= 0 && docs.slides.length > docs.currentSlide) {
                    background = docs.slides[docs.currentSlide].backgroundAssetId || ""
                }
                backgroundCombo.currentIndex = 0
                for (var j = 0; j < docs.assets.length; ++j) {
                    if (docs.assets[j].id === background) {
                        backgroundCombo.currentIndex = j + 1
                        break
                    }
                }
                // The fit is the slide's, spelled by the model; an
                // unresolvable value leaves the combo where it is rather
                // than pretending to select a name that does not exist.
                var fit = docs.currentBackgroundFit
                fitCombo.currentIndex = fit === "blur" ? 2 : (fit === "contain" ? 1 : 0)
            }
            Component.onCompleted: sync()
            Connections {
                target: docs
                function onDocumentsChanged() { stylePanel.sync() }
                function onNavigationChanged() { stylePanel.sync() }
            }
            Connections {
                target: editPage
                function onVisibleChanged() { if (editPage.visible) stylePanel.sync() }
            }

            Text {
                color: "#8f93a5"
                font.pixelSize: 10
                text: "STYLE"
            }

            ComboBox {
                id: styleCombo
                objectName: "styleCombo"
                width: parent.width
                textRole: "name"
                model: [{ id: "", name: "— show default —" }].concat(docs.styles)
                // Choosing a style overrides the current slide with it;
                // the first row (empty id) hands the slide back.
                onActivated: function (index) {
                    if (!docs.applyStyleToCurrentSlide(stylePanel.styleIdAt(index))) {
                        stylePanel.sync()
                    }
                }
            }

            Row {
                spacing: 6
                PanelButton {
                    objectName: "styleNewButton"
                    text: "New"
                    // Forks what the slide draws now for this slide only.
                    onClicked: docs.createStyleFromCurrent(
                                   fieldStyleName.text.trim() || "Style")
                }
                PanelButton {
                    objectName: "styleDefaultButton"
                    text: "Show default"
                    enabled: stylePanel.targetId() !== ""
                    onClicked: docs.setShowDefaultStyle(stylePanel.targetId())
                }
                PanelButton {
                    objectName: "styleFollowButton"
                    text: "Follow show"
                    enabled: docs.currentStyleIsOverride
                    onClicked: docs.applyStyleToCurrentSlide("")
                }
            }

            PanelField {
                id: fieldStyleName
                objectName: "styleNameField"
                width: parent.width
                onEditingFinished: stylePanel.commit()
            }

            Row {
                spacing: 8
                Column {
                    spacing: 2
                    Text {
                        color: "#8f93a5"
                        font.pixelSize: 10
                        text: "TEXT COLOUR"
                    }
                    PanelField {
                        id: fieldTextColor
                        objectName: "styleColourField"
                        width: 116
                        onEditingFinished: stylePanel.commit()
                    }
                }
                Column {
                    spacing: 2
                    Text {
                        color: "#8f93a5"
                        font.pixelSize: 10
                        text: "BACKGROUND"
                    }
                    PanelField {
                        id: fieldBackColour
                        objectName: "styleBackField"
                        width: 116
                        onEditingFinished: stylePanel.commit()
                    }
                }
            }

            // The background's second colour (task 6.17c3): empty keeps
            // the flat fill above; a #rgb paints the gradient's bottom.
            // Its own full-width row — the two colour fields above fill
            // the panel between them.
            Column {
                spacing: 2
                width: stylePanel.width
                Text {
                    color: "#8f93a5"
                    font.pixelSize: 10
                    text: "BACKGROUND GRADIENT (EMPTY = FLAT)"
                }
                PanelField {
                    id: fieldBackGradient
                    objectName: "styleBackGradientField"
                    width: parent.width
                    onEditingFinished: stylePanel.commit()
                }
            }

            Row {
                spacing: 8
                Column {
                    spacing: 2
                    Text {
                        color: "#8f93a5"
                        font.pixelSize: 10
                        text: "FONT"
                    }
                    PanelField {
                        id: fieldFontFamily
                        objectName: "styleFontField"
                        width: 156
                        onEditingFinished: stylePanel.commit()
                    }
                }
                Column {
                    spacing: 2
                    Text {
                        color: "#8f93a5"
                        font.pixelSize: 10
                        text: "SIZE"
                    }
                    PanelField {
                        id: fieldStyleSize
                        objectName: "styleSizeField"
                        width: 60
                        onEditingFinished: stylePanel.commit()
                    }
                }
            }

            Row {
                spacing: 8
                Row {
                    spacing: 4
                    Repeater {
                        model: [
                            { key: "left", label: "L" },
                            { key: "center", label: "C" },
                            { key: "right", label: "R" },
                        ]
                        delegate: PanelButton {
                            required property var modelData
                            objectName: "styleAlign" + modelData.key
                            text: modelData.label
                            highlighted: (docs.currentStyle.align || "center") === modelData.key
                            onClicked: stylePanel.commit(modelData.key)
                        }
                    }
                }
                Column {
                    spacing: 2
                    Text {
                        color: "#8f93a5"
                        font.pixelSize: 10
                        text: "LINE SPACING"
                    }
                    PanelField {
                        id: fieldLineSpacing
                        objectName: "styleSpacingField"
                        width: 60
                        onEditingFinished: stylePanel.commit()
                    }
                }
            }

            Text {
                color: "#8f93a5"
                font.pixelSize: 10
                text: "BACKGROUND IMAGE"
            }

            ComboBox {
                id: backgroundCombo
                objectName: "backgroundCombo"
                width: parent.width
                textRole: "name"
                model: [{ id: "", name: "None" }].concat(docs.assets)
                onActivated: function (index) {
                    var assetId = ""
                    if (index > 0 && index - 1 < docs.assets.length) {
                        assetId = docs.assets[index - 1].id
                    }
                    if (!docs.setSlideBackground(assetId)) {
                        stylePanel.sync()
                    }
                }
            }

            // Cover fills the frame and crops; contain keeps the whole
            // picture and lets the fill show in the bars (6.17c1 — the
            // choice the model stores and the worker honours, one undo
            // step, refused names landing on the status line); blur puts
            // the contained picture over a blurred cover backdrop of
            // itself at the reference's 0.3 (6.17c2 — refused on a video
            // background, and the sync below snaps the combo back).
            Text {
                color: "#8f93a5"
                font.pixelSize: 10
                text: "BACKGROUND FIT"
            }

            ComboBox {
                id: fitCombo
                objectName: "backgroundFitCombo"
                width: parent.width
                model: ["Fill and crop (cover)", "Fit whole picture (contain)",
                        "Blurred backdrop (blur)"]
                onActivated: function (index) {
                    var name = index === 2 ? "blur" : (index === 1 ? "contain" : "cover")
                    if (!docs.setSlideBackgroundFit(name)) {
                        stylePanel.sync()
                    }
                }
            }
        }
    }

    /* ------------------------------------------------------------------
     * The settings page (task 4.5): the essential settings, persisted
     * through AppSettings — presenter number keys and the F11
     * fullscreen — plus a cheat sheet of the shortcuts the keyboard
     * flows rely on, and the service information rows. The reference
     * keeps its shortcuts hardcoded and undocumented; documenting them
     * in-app is what makes "core flows work by keyboard" learnable.
     * ------------------------------------------------------------------ */
    Item {
        id: settingsPage
        visible: root.page === "settings"
        anchors.left: leftDock.right
        anchors.right: rightDock.left
        anchors.top: topBar.bottom
        anchors.bottom: libraryDrawer.top
        anchors.margins: 12

        Column {
            width: Math.min(620, parent.width)
            spacing: 5

            Text {
                color: "#f0f0ff"
                font.pixelSize: 15
                font.bold: true
                text: "Settings"
            }

            Text {
                color: "#8f93a5"
                font.pixelSize: 10
                topPadding: 10
                text: "PRESENTER"
            }
            CheckBox {
                text: "Number keys play slides (1\u20139, 0 = ten)"
                font.pixelSize: 13
                checked: appSettings.numberKeysPresent
                onToggled: appSettings.numberKeysPresent = checked
            }
            Text {
                color: "#8f93a5"
                font.pixelSize: 11
                wrapMode: Text.WordWrap
                width: parent.width
                text: "Presenter keys always work: Space, PageDown, PageUp, arrows, Home and End walk the current show straight onto the output."
            }

            Text {
                color: "#8f93a5"
                font.pixelSize: 10
                topPadding: 10
                text: "WINDOW"
            }
            CheckBox {
                text: "Fullscreen (F11)"
                font.pixelSize: 13
                checked: appSettings.fullscreen
                onToggled: appSettings.fullscreen = checked
            }

            Text {
                color: "#8f93a5"
                font.pixelSize: 10
                topPadding: 10
                text: "SHORTCUTS"
            }
            Repeater {
                model: [
                    { key: "Space / PageDown / Right / F5",
                      what: "Next slide \u2014 straight to the output" },
                    { key: "PageUp / Left", what: "Previous slide" },
                    { key: "Home / End", what: "First / last slide" },
                    { key: ". ", what: "Clear everything on air" },
                    { key: "Esc", what: "Clear all \u2014 or restore; deselects first on the edit page" },
                    { key: "F1 / F2", what: "Clear the background / the slide" },
                    { key: "Ctrl+1 / Ctrl+2 / Ctrl+5", what: "Show, edit and settings pages" },
                    { key: "Ctrl+F", what: "Library search" },
                    { key: "Ctrl+D", what: "Duplicate the slide \u2014 opens the drawer when there is none" },
                    { key: "Ctrl+C / Ctrl+X / Ctrl+V", what: "Copy / cut / paste the slide \u2014 text fields keep theirs" },
                    { key: "Ctrl+Z \u00b7 Ctrl+Y / Ctrl+Shift+Z", what: "Undo \u00b7 redo" },
                    { key: "Ctrl+S", what: "Save the service file" },
                    { key: "Arrows / Ctrl+Arrows", what: "Nudge the selected box 1px / 10px (edit page)" },
                    { key: "Tab / Shift+Tab", what: "Select the next / previous text box (edit page)" },
                    { key: "F11", what: "Fullscreen" }
                ]
                delegate: Row {
                    required property var modelData
                    spacing: 14
                    Text {
                        width: 250
                        color: "#f0f0ff"
                        font.pixelSize: 12
                        text: modelData.key
                    }
                    Text {
                        color: "#8f93a5"
                        font.pixelSize: 12
                        text: modelData.what
                    }
                }
            }

            Text {
                color: "#8f93a5"
                font.pixelSize: 10
                topPadding: 10
                text: "SERVICE"
            }
            Row {
                spacing: 14
                Text {
                    width: 250
                    color: "#f0f0ff"
                    font.pixelSize: 12
                    text: "Version"
                }
                Text {
                    color: "#8f93a5"
                    font.pixelSize: 12
                    text: applicationVersion + " \u00b7 worker " + worker.status
                }
            }
            Row {
                spacing: 14
                Text {
                    width: 250
                    color: "#f0f0ff"
                    font.pixelSize: 12
                    text: "Service directory"
                }
                Text {
                    color: "#8f93a5"
                    font.pixelSize: 12
                    text: docs.serviceDirectory
                }
            }
            Row {
                spacing: 14
                Text {
                    width: 250
                    color: "#f0f0ff"
                    font.pixelSize: 12
                    text: "Recovery autosave"
                }
                Text {
                    color: "#8f93a5"
                    font.pixelSize: 12
                    text: "saved with every edit \u00b7 Ctrl+S writes the service file"
                }
            }
        }
    }

    /* ------------------------------------------------------------------
     * Bottom library drawer (task 4.2): the searchable Shows/Media strip.
     * Its height is WorkspacePanels' (300 open, 40 closed). The type is
     * named LibraryDrawer so it doesn't shadow QtQuick.Controls' Drawer.
     * ------------------------------------------------------------------ */
    LibraryDrawer {
        id: libraryDrawer
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
    }
}
