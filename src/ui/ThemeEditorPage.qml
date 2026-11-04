import QtQuick
import QtQuick.Controls
import QtQuick.Window

/*
 * The Theme Editor (task TH.4, themes plan): the theme list left (New,
 * Duplicate, double-click Rename, Delete), the inspector right (every
 * property of the selected theme), and centre a large live preview of a
 * built-in sample cue rendered in the selected theme through the worker's
 * own off-air lane (6.16a). The sample is a fixture, never a document:
 * clicking through the list asks the worker to draw each theme's picture of
 * the SAME words, and the audience output never moves. Only one page owns
 * that lane at a time because only one page is visible at a time — the same
 * contract the Edit page keeps (6.16a), including standing the target down
 * on the way out and showing an honest blank when no worker is connected.
 * Since task TF.4 the inspector also edits the theme's `scripture` reading
 * context — typography and a box only, never a colour and never a line
 * count; the "lines" note there is the worker's measurement, shown as
 * information (text-flow plan).
 * Task TV.2 (theme-editor review) gave the page the whole workspace: the
 * shell hides the docks for it (Main.qml), the columns are bounded rather
 * than floating off any window's edge — 220 px of navigation, a 16:9
 * canvas fitted to whatever is left, and a 300 px inspector that SCROLLS
 * so every control is reachable at the smallest supported window — and the
 * Songs/Bible navigation is a pair of live thumbnail tiles: the same
 * fixture the canvas walks, asked through the bounded off-air thumbnail
 * lane beside the big selection ask (two request kinds, two generations,
 * one worker).
 */
Item {
    id: themeEditor
    objectName: "themeEditorPage"

    // The theme under edit. "" means the service has none yet, and every
    // control that needs a target disables itself honestly on it.
    property string selectedId: ""
    // While a row is being renamed in place the list shows its field
    // instead of its label; one row at a time, owned here, so the commit
    // and the cancel each have exactly one home.
    property string renamingId: ""
    // The live record of the selection, re-read from the model's own list
    // so an edit (or a delete) repaints the inspector from truth.
    function rowFor(id) {
        if (id === "")
            return null
        for (const theme of docs.themes) {
            if (theme.id === id)
                return theme
        }
        return null
    }
    readonly property var row: rowFor(selectedId)

    function selectTheme(id) {
        if (id !== selectedId) {
            selectedId = id
            askForSamplePicture()
        }
    }

    /* --- theme views (task TV.1, theme-editor review) ---------------------
     * Songs is the theme's OWN look (the base fields and placement); Bible
     * is the theme's `scripture` text context — the same record the Bible
     * page paginates against. The spellings are the storage's: "" is the
     * song/default view and "scripture" is the Bible view (review R3).
     * Theme identity and view are independent properties — switching a
     * view never changes which theme is selected, never creates a context,
     * and never dirties the document: selecting an absent Bible view simply
     * previews the look it would inherit.
     */
    property string selectedView: ""
    function selectView(view) {
        if (view === themeEditor.selectedView)
            return
        // An unfinished gesture belongs to the view it started in: it is
        // cancelled, not committed under the new view's handles.
        themeEditor.placing = false
        themeEditor.dragMode = ""
        themeEditor.selectedView = view
        themeEditor.syncInspector()
        themeEditor.askForSamplePicture()
    }
    // The compiled fixture's PRIMARY box and typography for the selected
    // theme AND view — the overlay's and the fields' single truth, so the
    // handles can never frame a song picture beneath Bible edits (R4).
    property var viewLook: ({ok: false})
    function refreshViewLook() {
        if (themeEditor.selectedId === "") {
            themeEditor.viewLook = ({ok: false})
            return
        }
        themeEditor.viewLook = docs.themeSampleAppearance(themeEditor.selectedId,
                                                          themeEditor.selectedView)
    }
    // Re-pick the selection when the list moved under it — a delete, or
    // the first theme added to an empty service. Selection prefers the
    // worn theme: that is the one the operator sees through the show.
    function reconcileSelection() {
        const themes = docs.themes
        if (themes.some(t => t.id === selectedId))
            return
        if (docs.wornThemeId !== "" && themes.some(t => t.id === docs.wornThemeId))
            themeEditor.selectedId = docs.wornThemeId
        else
            themeEditor.selectedId = themes.length > 0 ? themes[0].id : ""
        syncInspector()
    }
    // The fields SHOW the selection's values by assignment, not binding:
    // a field the operator is typing in must not be overwritten mid-word,
    // and after a commit the model is re-read whole anyway. The row is
    // re-read through rowFor rather than taken from the `row` property
    // because this function runs from inside the documentsChanged emit:
    // at that moment the property's binding may not have re-run yet, and
    // the cached row would replay the numbers the write just replaced.
    function syncInspector() {
        const row = themeEditor.rowFor(themeEditor.selectedId)
        nameField.text = row !== null ? row.name : ""
        themeEditor.refreshViewLook()
        themeEditor.syncContext()
    }
    /* The style rows, the colours, the box fields, the effects and the
     * transition section now live in the shared inspector parts (task
     * CS.5) and re-sync themselves off the adapter's changed()/
     * targetChanged() — one vocabulary for every rail. This function
     * keeps what only the page owns: the Name row, the compiled fixture
     * the overlay draws, and the context toggle the record drives.
     */

    // Every inspector edit writes the record back WHOLE — editTheme's
    // contract that one omitted field cannot silently flatten a gradient.
    // A value the model refuses lands on the status line, not in the data.
    function applyField(name, value) {
        const row = themeEditor.row
        if (row === null)
            return
        docs.editTheme(row.id,
                       name === "name" ? value : row.name,
                       name === "textColor" ? value : row.textColor,
                       name === "backgroundColor" ? value : row.backgroundColor,
                       name === "backgroundGradient" ? value : row.backgroundGradient,
                       name === "fontFamily" ? value : row.fontFamily,
                       name === "fontSize" ? value : row.fontSize,
                       name === "align" ? value : row.align,
                       name === "lineSpacing" ? value : row.lineSpacing)
    }

    /* --- typography, effects and the family picker (task TS.4, shared in CS.5)
     * The style block is SHARED, so its controls live in the shared parts
     * and use the ONE-FIELD merge through the adapter rather than
     * re-spelling the whole record every keystroke: dragging the blur
     * slider lands ONE undo (commit on release, not per pixel), and a
     * field the operator tabs through without changing writes NOTHING. A
     * refused write snaps the control back to the record — a field showing
     * a number the model turned down is a field about to be trusted. The
     * family field stays a free-typed field always (it worked before the
     * worker could answer and still works disconnected), and the Browse
     * popup is WorkerFontPicker's: only a worker that named font.list can
     * fill it, and a requested family the worker cannot resolve is flagged
     * MISSING but the stored name is never rewritten.
     */

    /* --- the scripture reading context (task TF.4, text-flow plan) -------
     * The entry this theme carries under the name "scripture" decides how
     * a Bible passage is SET: typography and a box, and nothing else — no
     * colour (the theme's look is the theme's own) and never a line count,
     * because how many lines ride a slide is what the worker's measurement
     * says, not what anyone types here. The fields read and write that one
     * entry through the controller's verbs; an empty field is SILENCE (the
     * property falls through to the themes), and the "lines" note is the
     * same engine's answer for the same box the Bible page will paginate
     * into, shown as information only.
     */
    // The measure request id of the in-flight lines probe (0 = none).
    property int linesAskId: 0
    // Whether the selected theme carries the context (derived on every
    // sync; the toggle follows the record, never the other way round).
    property bool contextOn: false

    function themeRow() {
        return themeEditor.rowFor(themeEditor.selectedId)
    }
    function contextRecord() {
        if (themeEditor.selectedId === "" || themeEditor.themeRow() === null)
            return {set: false}
        return docs.themeContext(themeEditor.selectedId, "scripture")
    }
    // The look the passage actually resolves: the context where it speaks,
    // the theme and the landing box where it is silent. The lines probe
    // measures THIS, which is the same chain the Bible page measures.
    function contextTypography() {
        const row = themeEditor.themeRow()
        const ctx = themeEditor.contextRecord()
        return {
            size: ctx.fontSizeSet === true ? ctx.fontSize : (row !== null ? row.fontSize : 0),
            spacing: ctx.lineSpacingSet === true ? ctx.lineSpacing
                                                 : (row !== null ? row.lineSpacing : 1.25),
            box: ctx.boxSet === true ? Qt.rect(ctx.x, ctx.y, ctx.width, ctx.height)
                                     : themeEditor.boxFor(row)
        }
    }
    // Re-read the TOGGLE from the record — the commit, the More menu's
    // undo, a refused write: whatever moved, the toggle follows the record,
    // never its own last click. The context's typography/box/movement rows
    // re-sync through the shared parts' adapter connections (CS.5); the
    // lines probe re-asks because the entry's box may have moved.
    function syncContext() {
        const ctx = themeEditor.contextRecord()
        themeEditor.contextOn = ctx.set === true
        ctxToggle.checked = ctx.set === true
        themeEditor.contextPlaces = ctx.boxSet === true || ctx.insetSet === true
        linesAsk.restart()
    }
    // Toggle on CREATES the context from this theme's current look and box:
    // the reading starts as what the audience already sees, and the
    // operator narrows it to a lectern from there. Toggle off removes the
    // entry; a refusal (there was nothing to remove) leaves the toggle
    // exactly where the record says it is.
    function setContext(on) {
        const row = themeEditor.themeRow()
        if (row === null) {
            ctxToggle.checked = themeEditor.contextOn
            return
        }
        if (on) {
            const box = themeEditor.boxFor(row)
            const fields = {}
            if (row.fontFamily !== "")
                fields.fontFamily = row.fontFamily
            if (row.fontSize > 0)
                fields.fontSize = row.fontSize
            fields.lineSpacing = row.lineSpacing
            fields.align = row.align
            fields.box = {x: box.x, y: box.y, width: box.width, height: box.height}
            if (!docs.applyThemeContext(row.id, "scripture", fields))
                ctxToggle.checked = false
        } else if (!docs.removeThemeContext(row.id, "scripture")) {
            ctxToggle.checked = true
        }
    }
    // The Bible rail's "Theme's lyric box" (task TV.1): borrow the RECTANGLE
    // the theme itself places (or the offered default) into the view's
    // record — the adapter merges the whole entry and retires a stored
    // inset in the same edit, so this is one write like any other field.
    function copyThemeBox() {
        const box = themeEditor.boxFor(themeEditor.themeRow())
        if (!contextAdapter.commit("box", {x: box.x, y: box.y,
                                           width: box.width, height: box.height}))
            contextInspector.syncAll()  // the refusal wins: the fields go back
    }

    /* --- how a view moves (task TR.3, shared in CS.5) -----------------------
     * The Songs section edits the theme's own record and the Bible section
     * the `scripture` context's — the two seats TR.1's resolver walks, in
     * the order it walks them — and both now do it through the shared
     * TransitionControls part over the adapter: the picker's three choices,
     * ONE atomic record per commit, the numbers seeded from what the seat
     * below answers with, the source sentence naming that seat, Inherit as
     * the removal. The rules (disabled-not-erased numbers, shape-only QML
     * validation, no undo for a no-change commit) are the part's, and this
     * page still cannot present anything: the theme under edit may not be
     * on air at all.
     */

    /* --- the placement (themes plan amendment 2026-11-01) ---------------
     *
     * Where this theme puts a wearing slide's PRIMARY lyric box, edited with
     * the slide editor's own gestures: press inside to move, a corner to
     * resize, ONE write on release, and the numbers can be typed instead.
     * Absent is a real answer, so the overlay shows the built-in lyric box
     * as an OFFER while the theme places nothing, and the first drag (or the
     * first typed value) makes the placement exist. None of this reaches the
     * audience: the sample is a fixture, and the theme's own rectangle is all
     * that changes — the same boundary the colour fields keep.
     */
    readonly property bool hasPlacement: themeEditor.row !== null
                                         && themeEditor.row.placementSet === true
    // Whether the VIEW being edited states its own box: the Songs view's is
    // the theme's placement; the Bible view's is the context's box or inset
    // (re-derived from the record on every sync, like contextOn).
    property bool contextPlaces: false
    readonly property bool viewPlaces: themeEditor.selectedView === ""
                                       ? themeEditor.hasPlacement : themeEditor.contextPlaces
    // The gesture's live rectangle, in the 1920x1080 authored canvas.
    property bool placing: false
    property string dragMode: ""
    property real dragOriginX: 0
    property real dragOriginY: 0
    property real penX: 0
    property real penY: 0
    property real penW: 0
    property real penH: 0
    property real anchorX: 0
    property real anchorY: 0
    property real anchorW: 0
    property real anchorH: 0
    function clamp(value, lo, hi) {
        return value < lo ? lo : (value > hi ? hi : value)
    }
    function boxFor(row) {
        if (row !== null && row.placementSet)
            return Qt.rect(row.placement.x, row.placement.y,
                           row.placement.width, row.placement.height)
        return Qt.rect(160, 100, 1600, 880)
    }
    // What the overlay draws and the fields show: the live drag while a
    // gesture runs; the SELECTED VIEW's compiled rectangle otherwise — the
    // theme's own box (or the built-in lyric box offered) for Songs, and
    // the box the fixture really resolves for Bible (the context's box, its
    // inset shrunk, or the inherited landing box). The Edit canvas's rule,
    // one write per gesture.
    function viewBox() {
        const look = themeEditor.viewLook
        if (themeEditor.selectedView !== "" && look && look.ok === true)
            return Qt.rect(look.boxX, look.boxY, look.boxWidth, look.boxHeight)
        return themeEditor.boxFor(row)
    }
    readonly property rect shownBox: themeEditor.placing
            ? Qt.rect(themeEditor.penX, themeEditor.penY,
                      themeEditor.penW, themeEditor.penH)
            : themeEditor.viewBox()
    // Handles beat the interior, in preview pixels, with the Edit canvas's
    // 8 px tolerance and its 20 authored px floor: below that a box cannot
    // be grabbed back by its own handles.
    function hitTest(px, py) {
        if (themeEditor.row === null)
            return ""
        const zoom = placementLayer.zoom
        const box = themeEditor.shownBox
        const tol = 8
        const left = box.x * zoom
        const top = box.y * zoom
        const right = (box.x + box.width) * zoom
        const bottom = (box.y + box.height) * zoom
        if (Math.abs(px - left) <= tol && Math.abs(py - top) <= tol) return "tl"
        if (Math.abs(px - right) <= tol && Math.abs(py - top) <= tol) return "tr"
        if (Math.abs(px - left) <= tol && Math.abs(py - bottom) <= tol) return "bl"
        if (Math.abs(px - right) <= tol && Math.abs(py - bottom) <= tol) return "br"
        if (px >= left && px <= right && py >= top && py <= bottom) return "move"
        return ""
    }
    function beginGesture(mode) {
        const box = themeEditor.shownBox
        themeEditor.dragMode = mode
        themeEditor.anchorX = box.x
        themeEditor.anchorY = box.y
        themeEditor.anchorW = box.width
        themeEditor.anchorH = box.height
        themeEditor.penX = box.x
        themeEditor.penY = box.y
        themeEditor.penW = box.width
        themeEditor.penH = box.height
        themeEditor.placing = true
    }
    function moveGesture(dx, dy) {
        if (themeEditor.dragMode === "move") {
            themeEditor.penX = themeEditor.clamp(themeEditor.anchorX + dx, 0,
                                                 1920 - themeEditor.anchorW)
            themeEditor.penY = themeEditor.clamp(themeEditor.anchorY + dy, 0,
                                                 1080 - themeEditor.anchorH)
            return
        }
        const min = 20
        const right = themeEditor.anchorX + themeEditor.anchorW
        const bottom = themeEditor.anchorY + themeEditor.anchorH
        const x0 = (themeEditor.dragMode === "tl" || themeEditor.dragMode === "bl")
            ? themeEditor.clamp(themeEditor.anchorX + dx, 0, right - min)
            : themeEditor.anchorX
        const y0 = (themeEditor.dragMode === "tl" || themeEditor.dragMode === "tr")
            ? themeEditor.clamp(themeEditor.anchorY + dy, 0, bottom - min)
            : themeEditor.anchorY
        themeEditor.penX = x0
        themeEditor.penY = y0
        themeEditor.penW = right - x0
        themeEditor.penH = bottom - y0
    }
    // One history entry per gesture, on release — the Edit canvas's rule,
    // so dragging a theme's box is one undo and not one per pixel. The
    // gesture commits to the VIEW it happened in (task TV.1): Songs drags
    // write the theme's placement, Bible drags write the context's own box
    // (retiring any inset in the same edit, per the adapter's context
    // merge), and the
    // other view's record is left exactly as it was.
    function endGesture() {
        if (themeEditor.dragMode === "") {
            themeEditor.placing = false
            return
        }
        const id = themeEditor.selectedId
        themeEditor.placing = false
        themeEditor.dragMode = ""
        if (id === "")
            return
        if (themeEditor.selectedView !== "") {
            contextAdapter.commit("box", {
                x: Math.round(themeEditor.penX), y: Math.round(themeEditor.penY),
                width: Math.round(themeEditor.penW), height: Math.round(themeEditor.penH)})
        } else {
            docs.setThemePlacement(id, Math.round(themeEditor.penX), Math.round(themeEditor.penY),
                                   Math.round(themeEditor.penW), Math.round(themeEditor.penH))
        }
        themeEditor.syncInspector()
    }


    // The centre picture: the worker's own composition of the built-in
    // sample wearing the selected theme. Debounced like the Edit page's
    // ask — browsing is a click-track, and one ask costs a prepare and an
    // off-air draw.
    Timer {
        id: sampleAsk
        interval: 120
        repeat: false
        onTriggered: {
            if (!themeEditor.visible || themeEditor.selectedId === "") {
                worker.clearSelectionPreview()
                return
            }
            const json = docs.themeSampleSceneJson(themeEditor.selectedId,
                                                   themeEditor.selectedView)
            if (json !== "")
                worker.requestSelectionPreview(json)
        }
    }
    /* --- the view thumbnails (task TV.2, theme-editor review) -------------
     * The Songs/Bible tiles next to the list each show the worker's OWN
     * picture of the fixture that view compiles — the same two scenes the
     * big canvas walks, asked small. They ride the existing bounded
     * off-air thumbnail lane (6.16b) as ONE batch of two keyed scenes, so
     * browsing a theme replaces both pictures in one act; the big canvas
     * keeps its separate selection ask, its own debounce and its own
     * generation — neither lane can stale the other. Keyed by theme id,
     * because the batch describes the SELECTED theme, not the selected
     * view: whichever tile is up, both answers are wanted.
     */
    function viewThumbKey(view) {
        return "theme-view-" + (view === "" ? "songs" : "bible") + "-" + themeEditor.selectedId
    }
    // The ask follows the tile's picture (16:9 inside the 32..512 envelope
    // the wire accepts at the 640x360 slot this app streams at) exactly as
    // the slide band follows its cells — a size this page picks can never
    // be refused on the wire.
    function viewThumbPixels() {
        const w = Math.max(32, Math.min(512, Math.round(songsTile.picWidth)))
        return {w: w, h: Math.round(w * 9 / 16)}
    }
    // The batch the page is about to send — a pure function of the
    // selection, so the offscreen suite can pin the keys and the scenes
    // without a worker on the wire.
    function viewThumbBatch() {
        if (themeEditor.selectedId === "")
            return []
        return [
            {key: themeEditor.viewThumbKey(""),
             scene: docs.themeSampleSceneJson(themeEditor.selectedId, "")},
            {key: themeEditor.viewThumbKey("scripture"),
             scene: docs.themeSampleSceneJson(themeEditor.selectedId, "scripture")}
        ]
    }
    property int viewThumbRequests: 0
    property bool viewThumbAskedFor: false
    function requestViewThumbs() {
        ++themeEditor.viewThumbRequests
        if (!themeEditor.visible || !worker.thumbnailSupported)
            return
        const batch = themeEditor.viewThumbBatch()
        if (batch.length === 0) {
            worker.clearThumbnails()
            return
        }
        const px = themeEditor.viewThumbPixels()
        worker.requestThumbnails(batch, px.w, px.h)
    }
    Timer {
        id: viewThumbAsk
        interval: 90
        repeat: false
        onTriggered: themeEditor.requestViewThumbs()
    }
    function askForSamplePicture() {
        sampleAsk.restart()
        viewThumbAsk.restart()
    }

    // The inspector rail's scroll verbs. ScrollView does NOT put its
    // Flickable at "the content column's parent": between the column and the
    // Flickable that owns contentY sits the Flickable's own content item, so
    // asking the column's parent to scroll silently asks something that
    // cannot scroll (task TS.4 found it reaching for the rail's last
    // control). The Flickable is therefore found by asking each parent
    // whether it scrolls at all, and "reachable" stays a statement about the
    // END of that Flickable — the promise the workspace suite checks at the
    // smallest window (task TV.2).
    function inspectorFlickable() {
        for (let node = inspectorContent.parent; node !== null; node = node.parent) {
            if (node.contentY !== undefined && node.contentHeight !== undefined)
                return node
        }
        return null
    }
    function scrollInspectorToTop() {
        const rail = themeEditor.inspectorFlickable()
        if (rail !== null)
            rail.contentY = 0
    }
    function scrollInspectorToEnd() {
        const rail = themeEditor.inspectorFlickable()
        if (rail !== null) {
            // A Flickable does NOT clamp a contentY it is handed, so the end
            // is computed, not asserted: past the last row is a rail showing
            // blank paper with the controls somewhere above it.
            rail.contentY = Math.max(0, rail.contentHeight - rail.height)
        }
    }
    // Bring ONE named control of the rail into view. Reachability is a
    // promise about every control, not only about the ends — the Songs
    // section is now taller than the smallest window shows at once, so a
    // test (or an operator) that wants a particular field has to be able to
    // ask for it by name and get it inside the viewport, not a rail scrolled
    // to some other row.
    function findInRail(item, objectName) {
        if (item.objectName === objectName)
            return item
        const kids = item.children
        for (let i = 0; i < kids.length; ++i) {
            const hit = themeEditor.findInRail(kids[i], objectName)
            if (hit !== null)
                return hit
        }
        return null
    }
    function scrollInspectorTo(objectName) {
        const rail = themeEditor.inspectorFlickable()
        if (rail === null)
            return false
        const target = themeEditor.findInRail(inspectorContent, objectName)
        if (target === null)
            return false
        const top = target.mapToItem(inspectorContent, 0, 0).y
        const bottom = top + target.height
        const limit = Math.max(0, rail.contentHeight - rail.height)
        if (top < rail.contentY)
            rail.contentY = Math.max(0, top - 12)
        else if (bottom > rail.contentY + rail.height)
            rail.contentY = Math.min(limit, bottom - rail.height + 12)
        return true
    }
    // The measured lines the passage's box holds (information, never a
    // setting): one "M" at the resolved size and spacing, measured by the
    // worker's own paragraph engine — the same one the Bible page paginates
    // with — and the box height divided by the row it answers with.
    Timer {
        id: linesAsk
        interval: 240
        repeat: false
        onTriggered: {
            const look = themeEditor.contextTypography()
            if (!themeEditor.visible || !themeEditor.contextOn) {
                ctxLinesLabel.text = ""
                return
            }
            if (!worker.connected) {
                ctxLinesLabel.text = "not measured — the worker is not answering"
                return
            }
            // A size of 0 is the renderer's own default, which no probe
            // here can name — say so rather than divide by a guess.
            if (look.size <= 0) {
                ctxLinesLabel.text = "the renderer's default size decides"
                return
            }
            const id = worker.measureText([{
                key: "theme-editor-lines",
                lines: ["M"],
                fontSize: look.size,
                width: look.box.width,
                lineSpacing: look.spacing
            }])
            themeEditor.linesAskId = id
            if (id === 0)
                ctxLinesLabel.text = "not measured — the worker is not answering"
        }
    }
    Connections {
        target: worker
        function onTextMeasured(requestId, measured) {
            if (requestId !== themeEditor.linesAskId || measured.length === 0)
                return  // another page's batch; the editor asked nothing
            themeEditor.linesAskId = 0
            const rowHeight = Number(measured[0].height)
            const box = themeEditor.contextTypography().box
            ctxLinesLabel.text = rowHeight > 0
                    ? "the box holds ≈ " + Math.floor(box.height / rowHeight)
                      + " lines of this size — how many ride is what the passage measures"
                    : ""
        }
        function onTextMeasureFailed(requestId, reason) {
            if (requestId !== themeEditor.linesAskId)
                return
            themeEditor.linesAskId = 0
            ctxLinesLabel.text = "not measured: " + reason
        }
        // The lane opened while the page is up (the worker answered the
        // capability question late): the tiles are owed their batch, and
        // this is the only hook that says so. One ask per stretch of
        // support — ShowPage keeps the same flag idiom.
        function onPreviewChanged() {
            if (themeEditor.visible && worker.thumbnailSupported
                    && !themeEditor.viewThumbAskedFor) {
                themeEditor.viewThumbAskedFor = true
                viewThumbAsk.restart()
            }
        }
    }
    Connections {
        target: docs
        function onDocumentsChanged() {
            themeEditor.reconcileSelection()
            // Re-derive the toggle from the model on every change: this is
            // the hook that keeps it honest when the More menu, not this
            // page, moved the current theme. (The user's own click already
            // broke the binding; this reassignment is what replaces it.)
            useToggle.checked = docs.wornThemeId !== ""
                                && docs.wornThemeId === themeEditor.selectedId
            // The inspector reads the RECORD: whatever moved it — this
            // page's own commit, "No placement", an undo — the fields and
            // the overlay follow, so a stale number can never outlive the
            // write it was typed for.
            if (themeEditor.visible) {
                themeEditor.syncInspector()
                themeEditor.askForSamplePicture()
            }
        }
    }
    onVisibleChanged: {
        if (themeEditor.visible) {
            themeEditor.reconcileSelection()
            // Shown pages read the RECORD: documents may have moved while
            // this page slept (another page, an undo, the More menu), and
            // a selection that never changed fires no selectedIdChanged to
            // re-sync the assigned fields. The overlay follows by binding;
            // the fields only follow through here.
            themeEditor.syncInspector()
            themeEditor.askForSamplePicture()
        } else {
            sampleAsk.stop()
            linesAsk.stop()
            viewThumbAsk.stop()
            worker.clearSelectionPreview()
            // The thumbnail lane belongs to whichever page is up: stand
            // the tiles' batch down on the way out (the Show band re-asks
            // its own batch as it becomes visible, and the two asks never
            // live in the same moment).
            worker.clearThumbnails()
            themeEditor.viewThumbAskedFor = false
            themeEditor.renamingId = ""
            // TV.2: mode exit cancels an unfinished gesture rather than
            // letting a half-dragged rectangle outlive the page, and the
            // keyboard leaves with the page too. Releasing only the field
            // the operator typed in hands the focus to the inspector's
            // ScrollView Pane — it catches the fall through the focus
            // chain — and the window then recomputes the chain around
            // whatever still has `focus` true, undoing a bare acquisition.
            // So the page releases every one of its own holders and gives
            // the window root the keyboard, and the shell's handover
            // (Main.qml onPageChanged) lands on ground that stays clear.
            const win = themeEditor.Window.window
            if (win) {
                for (let guard = 0; guard < 4; ++guard) {
                    const heldNow = win.activeFocusItem
                    if (!heldNow)
                        break
                    let inside = false
                    for (let up = heldNow; up; up = up.parent)
                        if (up === themeEditor) { inside = true; break }
                    if (!inside)
                        break
                    heldNow.focus = false
                    win.contentItem.forceActiveFocus()
                }
            }
            themeEditor.placing = false
            themeEditor.dragMode = ""
        }
    }
    Component.onCompleted: {
        themeEditor.reconcileSelection()
        themeEditor.syncInspector()
        themeEditor.pinRailNames()
    }
    // The automation names this suite (and assistive technology) read are
    // the page's promise, not the parts' defaults: each shared row keeps
    // the exact identifier it carried before the CS.5 extraction, assigned
    // here once so a rename is one line that says which part moved.
    function pinRailNames() {
        const b = songsInspector.backgroundControls
        b.textColorObjectName = "themeEditorColourField"
        b.backColorObjectName = "themeEditorBackField"
        b.gradientObjectName = "themeEditorGradientField"
        const st = songsInspector.styleControls
        st.sectionTitle = "Typography & effects"
        st.fontObjectName = "themeEditorFontField"
        st.fontBrowseObjectName = "themeEditorBrowseFonts"
        st.fontStatusObjectName = "themeEditorFontStatus"
        st.fontPopupObjectName = "themeEditorFontPopup"
        st.fontSearchObjectName = "themeEditorFontSearch"
        st.fontListStatusObjectName = "themeEditorFontListStatus"
        st.fontListObjectName = "themeEditorFontList"
        st.fontRowObjectName = "themeEditorFontRow"
        st.fontCancelObjectName = "themeEditorFontCancel"
        st.sizeObjectName = "themeEditorSizeField"
        st.alignObjectName = "themeEditorAlign"
        st.spacingObjectName = "themeEditorSpacingField"
        st.weightObjectName = "themeEditorWeight"
        st.weightInheritObjectName = "themeEditorWeightInherit"
        st.slantObjectName = "themeEditorSlant"
        st.slantInheritObjectName = "themeEditorSlantInherit"
        st.fitObjectName = "themeEditorFit"
        st.fitInheritObjectName = "themeEditorFitInherit"
        st.minSizeObjectName = "themeEditorMinSize"
        st.minSizeInheritObjectName = "themeEditorMinSizeInherit"
        const g = songsInspector.geometryControls
        g.xObjectName = "themeEditorPlaceX"
        g.yObjectName = "themeEditorPlaceY"
        g.wObjectName = "themeEditorPlaceW"
        g.hObjectName = "themeEditorPlaceH"
        g.clearObjectName = "themeEditorNoPlacement"
        const fx = songsInspector.effectsControls
        fx.strokeCheckObjectName = "themeEditorStrokeEnable"
        fx.strokeOffObjectName = "themeEditorStrokeInherit"
        fx.strokeColourObjectName = "themeEditorStrokeColour"
        fx.strokeWidthObjectName = "themeEditorStrokeWidth"
        fx.shadowCheckObjectName = "themeEditorShadowEnable"
        fx.shadowOffObjectName = "themeEditorShadowInherit"
        fx.shadowColourObjectName = "themeEditorShadowColour"
        fx.shadowOpacityObjectName = "themeEditorShadowOpacity"
        fx.shadowOffsetXObjectName = "themeEditorShadowOffsetX"
        fx.shadowOffsetYObjectName = "themeEditorShadowOffsetY"
        fx.shadowBlurObjectName = "themeEditorShadowBlur"
        const mv = songsInspector.transitionControls
        mv.kindObjectName = "themeEditorTransitionKind"
        mv.inheritObjectName = "themeEditorTransitionInherit"
        mv.wordsObjectName = "themeEditorTransitionWords"
        mv.backObjectName = "themeEditorTransitionMedia"
        mv.sourceObjectName = "themeEditorTransitionSource"
        mv.noteObjectName = "themeEditorTransitionNote"
        mv.silentSuffix = " — a theme lower in a wearing slide's chain answers first"
        mv.note = "Words covers the text and any foreground picture; the background is "
                + "a colour, a gradient or a still image. A background that is a VIDEO "
                + "still cuts either way, so no background number dissolves one clip "
                + "into the next. What is set here is the NEXT cue wearing this theme: "
                + "a fade already crossing keeps the lengths it was given at Go."

        const cst = contextInspector.styleControls
        cst.sparse = true
        cst.showStyleBlock = false  // the reading record sets type basics
                                  // only; weight/slant/fit stay the theme's
        cst.fontShowStatus = false
        cst.fontLabel = "Reading font (\"\" = theme's)"
        cst.sizeLabel = "Reading size (0 = renderer, \"\" = theme's)"
        cst.alignLabel = "Reading alignment"
        cst.spacingLabel = "Reading line spacing (0.25 to 8, \"\" = theme's)"
        cst.fontObjectName = "themeEditorContextFont"
        cst.fontBrowseObjectName = "themeEditorContextFontBrowse"
        cst.fontStatusObjectName = "themeEditorContextFontStatus"
        cst.fontPopupObjectName = "themeEditorContextFontPopup"
        cst.sizeObjectName = "themeEditorContextSize"
        cst.alignObjectName = "themeEditorContextAlign"
        cst.spacingObjectName = "themeEditorContextSpacing"
        const cg = contextInspector.geometryControls
        cg.label = "Bible box (x, y, w, h in 1920×1080)"
        cg.xObjectName = "themeEditorContextX"
        cg.yObjectName = "themeEditorContextY"
        cg.wObjectName = "themeEditorContextW"
        cg.hObjectName = "themeEditorContextH"
        cg.clearLabel = "No box"
        cg.extraActionLabel = "Theme's lyric box"
        cg.extraObjectName = "themeEditorContextBoxTheme"
        cg.clearObjectName = "themeEditorContextBoxClear"
        cg.showStatus = false
        cg.extraClicked.connect(themeEditor.copyThemeBox)
        const cmv = contextInspector.transitionControls
        cmv.kindObjectName = "themeEditorContextTransitionKind"
        cmv.inheritObjectName = "themeEditorContextTransitionInherit"
        cmv.wordsObjectName = "themeEditorContextTransitionWords"
        cmv.backObjectName = "themeEditorContextTransitionMedia"
        cmv.sourceObjectName = "themeEditorContextTransitionSource"
        cmv.noteObjectName = "themeEditorContextTransitionNote"
        cmv.silentSuffix = " — the theme carrying this view answers before the service"
        cmv.note = "a passage's view can move differently from the song's words; a "
                 + "Cut here is a decision, not an absence, and it stops the walk. "
                 + "A background that is a VIDEO still cuts whatever these say."

        // The sparse/section flags changed what the syncs above drew; one
        // pass each now that the rails are configured.
        songsInspector.syncAll()
        contextInspector.syncAll()
    }
    onSelectedIdChanged: themeEditor.syncInspector()

    /* --- the list (bounded, task TV.2) -----------------------------------
     * The footer below used to sit under a fixed-height list: at a short
     * window the New row, the view navigator and the notes slid past the
     * bottom edge of everything. Now the footer is anchored to the bottom
     * and the LIST takes the remainder — whatever the window, every button
     * has its place and its click.
     */

    // One shape for both view tiles (task TV.2): a 16:9 worker's picture,
    // a label, and the selected view stated by the label colour and the
    // border. The tile IS the button, so the objectNames TV.1 pinned are
    // the whole click target, and the keyboard reaches them like the old
    // plain buttons did.
    component ViewTile: Button {
        id: tileButton
        required property string view    // "" for Songs, "scripture" for Bible
        required property string label
        // The picture width the thumbnail ask follows (viewThumbPixels).
        readonly property real picWidth: 99
        flat: true
        padding: 4
        Accessible.name: tileButton.label
        contentItem: Column {
            spacing: 3
            // The worker's own picture of THIS view's fixture — the same key
            // the batch asked with — in the shared surface. With no lane open
            // the tile is an honest black frame and a glyph, never a QML
            // guess at the theme's look.
            SlidePreviewSurface {
                width: tileButton.picWidth
                height: tileButton.picWidth * 9 / 16
                controller: worker
                target: "thumbnail"
                key: themeEditor.selectedId === ""
                     ? "" : themeEditor.viewThumbKey(tileButton.view)
                pixelsVisible: worker.thumbnailSupported
                borderColor: themeEditor.selectedView === tileButton.view
                             ? "#1685f8" : "#4a4a4a"
                borderWidth: 1
                placeholder: tileButton.view === "" ? "\u266a" : "\u271d"
                placeholderVisible: !worker.thumbnailSupported
                placeholderColor: "#888888"
                placeholderPixelSize: 18
            }
            Text {
                width: tileButton.picWidth
                horizontalAlignment: Text.AlignHCenter
                text: tileButton.label
                color: themeEditor.selectedView === tileButton.view
                       ? "#8ec9f5" : "#cccccc"
                font.pixelSize: 12
                font.bold: themeEditor.selectedView === tileButton.view
            }
        }
        background: Rectangle {
            radius: 4
            color: tileButton.hovered ? "#2c2c2e" : "transparent"
        }
        onClicked: themeEditor.selectView(tileButton.view)
    }

    Item {
        id: listColumn
        objectName: "themeEditorNav"
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: 220

        Text {
            id: navTitle
            anchors.left: parent.left
            anchors.top: parent.top
            text: "THEMES"; color: "#bbbbbb"; font.pixelSize: 11; font.bold: true
        }
        Column {
            id: navFooter
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            spacing: 8
            // New/Duplicate/Delete own a Flow: the row fits 220 px today,
            // and if a translation or a future button outgrows it the row
            // wraps instead of walking off the workspace.
            Flow {
                objectName: "themeEditorActions"
                width: parent.width
                spacing: 6
                Button {
                    objectName: "themeEditorNew"
                    text: "New"
                    onClicked: docs.createBlankTheme("New theme")
                }
                Button {
                    objectName: "themeEditorDuplicate"
                    text: "Duplicate"
                    enabled: themeEditor.selectedId !== ""
                    onClicked: docs.duplicateTheme(themeEditor.selectedId)
                }
                Button {
                    objectName: "themeEditorDelete"
                    text: "Delete"
                    enabled: themeEditor.selectedId !== ""
                             // The seeded Default theme (amendment 2026-11-02)
                             // is the floor every chain resolves through:
                             // editable like any theme, but never deleted.
                             && !(themeEditor.row && themeEditor.row.builtin === true)
                    onClicked: docs.deleteTheme(themeEditor.selectedId)
                }
            }
            // The view navigator (task TV.1, theme-editor review): which half
            // of the theme the canvas and inspector edit — Songs is the theme's
            // own look, Bible is its `scripture` context. Picking a view is
            // pure browsing: it never creates a record, never edits the theme,
            // and never marks the document dirty. Since TV.2 the two entries
            // are live thumbnails of exactly what each view will draw.
            Row {
                id: viewTiles
                objectName: "themeEditorViewTiles"
                spacing: 6
                ViewTile {
                    id: songsTile
                    objectName: "themeViewSongs"
                    view: ""
                    label: "Songs"
                }
                ViewTile {
                    objectName: "themeViewBible"
                    view: "scripture"
                    label: "Bible"
                }
            }
            Text {
                objectName: "themeEditorViewNote"
                width: parent.width
                wrapMode: Text.WordWrap
                color: "#8ec9f5"
                font.pixelSize: 11
                text: themeEditor.selectedView === ""
                      ? "Viewing SONGS — the theme's own look, which the Bible "
                        + "view inherits from"
                      : "Viewing BIBLE — the scripture reading context; what it "
                        + "does not say, the Songs look answers"
            }
            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                color: "#b0b0b0"
                font.pixelSize: 11
                text: "Double-click a name to rename it. Deleting clears every "
                      + "reference to the theme; Ctrl+Z brings it back."
            }
        }
        ListView {
            id: themeList
            objectName: "themeEditorList"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: navTitle.bottom
            anchors.topMargin: 4
            anchors.bottom: navFooter.top
            anchors.bottomMargin: 8
            clip: true
            model: docs.themes
            delegate: Rectangle {
                id: themeRow
                required property var modelData
                objectName: "themeEditorRow"
                width: themeList.width
                height: 34
                radius: 4
                color: themeEditor.selectedId === themeRow.modelData.id ? "#626265"
                       : rowMouse.containsMouse ? "#59595b" : "transparent"
                property bool editing: themeEditor.renamingId === themeRow.modelData.id
                Row {
                    anchors.left: parent.left
                    anchors.leftMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 6
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 14
                        color: "#8ec9f5"
                        font.pixelSize: 13
                        text: docs.wornThemeId === themeRow.modelData.id ? "\u2713" : ""
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: !themeRow.editing
                        text: themeRow.modelData.name
                        color: "#eeeeee"
                        font.pixelSize: 13
                    }
                    PanelField {
                        objectName: "themeEditorRenameField"
                        visible: themeRow.editing
                        width: 140
                        text: themeRow.modelData.name
                        onVisibleChanged: if (visible) forceActiveFocus()
                        onEditingFinished: {
                            const clean = text.trim()
                            themeEditor.renamingId = ""
                            if (clean !== "" && clean !== themeRow.modelData.name) {
                                const model = themeRow.modelData
                                docs.editTheme(model.id, clean, model.textColor,
                                               model.backgroundColor, model.backgroundGradient,
                                               model.fontFamily, model.fontSize, model.align,
                                               model.lineSpacing)
                            }
                        }
                        Keys.onEscapePressed: themeEditor.renamingId = ""
                    }
                }
                MouseArea {
                    id: rowMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    // Selection is the single click; the rename is its own
                    // gesture, because Qt emits `clicked` once for a
                    // double-click too — on the release, without a flag —
                    // so a check on the press could only ever live here.
                    onClicked: themeEditor.selectTheme(themeRow.modelData.id)
                    onDoubleClicked: themeEditor.renamingId = themeRow.modelData.id
                }
            }
        }
    }

    /* --- the centre: header, sample, honesty line -----------------------
     * The canvas is a 16:9 picture that FITS — `min(width, height·16/9)`
     * of the space the columns leave, centred in it (task TV.2). The
     * overlay inside it transforms by the frame's own width/1920, so the
     * handles track the box at whatever size and position the window
     * settled on; there is no second geometry anywhere.
     */

    Column {
        id: centerColumn
        anchors.left: listColumn.right
        anchors.leftMargin: 12
        anchors.right: inspectorScroll.left
        anchors.rightMargin: 12
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        spacing: 10

        Row {
            id: editorHeader
            spacing: 14
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "Theme Editor"
                color: "#eeeeee"
                font.pixelSize: 18
                font.bold: true
            }
            CheckBox {
                id: useToggle
                objectName: "themeEditorUseToggle"
                anchors.verticalCenter: parent.verticalCenter
                text: "Use as current theme"
                font.pixelSize: 12
                enabled: themeEditor.selectedId !== ""
                // The toggle reads the model, not its own press: the More
                // menu can put a theme on or take it off from anywhere, and
                // a refusal must not leave a checked box claiming otherwise.
                checked: docs.wornThemeId !== ""
                         && docs.wornThemeId === themeEditor.selectedId
                onToggled: {
                    docs.useThemeAsCurrent(checked ? themeEditor.selectedId : "")
                    checked = docs.wornThemeId !== ""
                              && docs.wornThemeId === themeEditor.selectedId
                }
            }
        }

        // The centre canvas (task CS.2): a SlidePreviewSurface on the
        // Selection target — this theme's sample rendered by the worker —
        // with the placement chrome riding the fitted content rectangle.
        SlidePreviewSurface {
            id: previewFrame
            objectName: "themeEditorPreview"
            // TV.2: fit the 16:9 picture to the room the columns leave —
            // both bounds at once, centred — and never more than the
            // height less the header row, the note and the honesty line.
            readonly property real chrome: editorHeader.height + sampleNote.height
                                           + (statusLine.visible ? statusLine.height : 0)
                                           + centerColumn.spacing * 3
            width: Math.min(centerColumn.width,
                            Math.max(160, (centerColumn.height - chrome) * 16 / 9))
            height: width * 9 / 16
            x: (centerColumn.width - width) / 2
            controller: worker
            target: "selection"
            borderColor: "#4a4a4a"
            borderWidth: 0
            clipOverlays: true
            // No picture (no worker, the first ask still in flight): the
            // frame says which, rather than painting a QML guess at it.
            placeholder: themeEditor.selectedId === "" ? "No theme selected"
                  : worker.selectionReady ? ""
                  : (worker.statusNeedsAttention ? "Preview unavailable"
                                                 : "Waiting for the preview")

            // The lyric box this theme places (themes plan amendment
            // 2026-11-01), drawn over the sample at the same 1920-unit scale
            // the Edit canvas uses. It is chrome over the worker's own
            // picture, never a second layout of the words — and with no
            // placement on the theme it is a dashed OFFER, the box the
            // audience would draw today, waiting to be moved into being.
            Item {
                id: placementLayer
                anchors.fill: parent
                enabled: themeEditor.row !== null
                visible: enabled
                readonly property real zoom: width / 1920
                readonly property rect box: themeEditor.shownBox

                Rectangle {
                    x: placementLayer.box.x * placementLayer.zoom
                    y: placementLayer.box.y * placementLayer.zoom
                    width: placementLayer.box.width * placementLayer.zoom
                    height: placementLayer.box.height * placementLayer.zoom
                    color: "transparent"
                    // A Rectangle border is only ever solid, and the solid/dashed
                    // difference here is the whole message — "this is the record"
                    // versus "this is only the box you are being offered". Canvas
                    // is the one QtQuick type that dashes a stroke without a new
                    // module import, so the outline is its own honest paint.
                    Canvas {
                        anchors.fill: parent
                        readonly property bool placed: themeEditor.viewPlaces
                        onPlacedChanged: requestPaint()
                        onWidthChanged: requestPaint()
                        onHeightChanged: requestPaint()
                        onPaint: {
                            const ctx = getContext("2d")
                            ctx.clearRect(0, 0, width, height)
                            ctx.strokeStyle = placed ? "#1685f8" : "#8ec9f5"
                            ctx.lineWidth = 2
                            ctx.setLineDash(placed ? [] : [6, 4])
                            ctx.strokeRect(1, 1, width - 2, height - 2)
                        }
                    }
                    Text {
                        objectName: "themeEditorBoxLabel"
                        anchors.left: parent.left
                        anchors.bottom: parent.top
                        anchors.bottomMargin: 3
                        color: themeEditor.viewPlaces ? "#1685f8" : "#8ec9f5"
                        font.pixelSize: 11
                        text: themeEditor.selectedView === ""
                              ? (themeEditor.viewPlaces ? "lyric box"
                                                        : "no placement — drag to place")
                              : (themeEditor.viewPlaces ? "Bible box"
                                                        : "no Bible box — drag to place")
                    }
                    Repeater {
                        model: [Qt.point(0, 0), Qt.point(1, 0), Qt.point(0, 1), Qt.point(1, 1)]
                        delegate: Rectangle {
                            required property var modelData
                            width: 8
                            height: 8
                            color: themeEditor.viewPlaces ? "#1685f8" : "#8ec9f5"
                            border.color: "#202020"
                            x: placementLayer.box.x * placementLayer.zoom
                                + (modelData.x === 0 ? -width / 2
                                                     : placementLayer.box.width
                                                       * placementLayer.zoom - width / 2)
                            y: placementLayer.box.y * placementLayer.zoom
                                + (modelData.y === 0 ? -height / 2
                                                     : placementLayer.box.height
                                                       * placementLayer.zoom - height / 2)
                        }
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.LeftButton
                    onPressed: function (mouse) {
                        const mode = themeEditor.hitTest(mouse.x, mouse.y)
                        if (mode !== "") {
                            themeEditor.beginGesture(mode)
                            themeEditor.dragOriginX = mouse.x
                            themeEditor.dragOriginY = mouse.y
                        }
                    }
                    onPositionChanged: function (mouse) {
                        if (themeEditor.dragMode !== "")
                            themeEditor.moveGesture((mouse.x - themeEditor.dragOriginX)
                                                    / placementLayer.zoom,
                                                    (mouse.y - themeEditor.dragOriginY)
                                                    / placementLayer.zoom)
                    }
                    onReleased: themeEditor.endGesture()
                    onCanceled: themeEditor.endGesture()
                }
            }
        }
        Text {
            id: sampleNote
            width: parent.width
            wrapMode: Text.WordWrap
            color: "#b0b0b0"
            font.pixelSize: 11
            text: "A built-in sample cue wearing this theme — the same "
                  + "picture on every row. Browsing themes never touches "
                  + "the audience output."
        }
        Text {
            id: statusLine
            objectName: "themeEditorStatus"
            width: parent.width
            wrapMode: Text.WordWrap
            color: "#d9a441"
            font.pixelSize: 11
            visible: text !== ""
            text: docs.themes.length === 0 && themeEditor.selectedId === ""
                  ? "No themes yet — New makes one." : docs.status
        }
    }

    /* --- the inspector ---------------------------------------------------
     *
     * The inspector edits one VIEW at a time (task TV.1): the SONGS section
     * holds the theme's own look — the values the Bible view inherits from —
     * and the BIBLE section holds the theme's `scripture` context. The
     * sections follow `selectedView`; the sync writes every field either
     * way so whichever section wakes already holds the record.
     *
     * Task TV.2 put ALL of it in a ScrollView at a bounded 300 px: the
     * effect controls TS.4 will add make this column longer than any
     * window is tall, and an operator who cannot reach a control cannot
     * use it — so the rail scrolls, clips, and its last field is a real
     * click target at the smallest supported window, not a promise. The
     * rail never grabs the keyboard for itself: the shell's page-change
     * handover (Main.qml) owns the focus at every mode switch.
     */

    /* --- the inspector read model (task CS.4, wired by CS.5) ----------------
     * Two explicit targets, two rails: the Songs column edits the THEME,
     * the Bible column the `scripture` context CARRIED BY the selected
     * theme. Browsing (a new theme, a view switch) only moves `target`;
     * every row re-reads itself off the adapter, so the rails cannot
     * disagree with the records a Go would resolve against.
     */
    InspectorAdapter {
        id: themeAdapter
        document: docs
        target: ({kind: "theme", id: themeEditor.selectedId})
    }
    InspectorAdapter {
        id: contextAdapter
        document: docs
        target: ({kind: "context", id: themeEditor.selectedId, view: "scripture"})
    }
    // The context-property worker, named once at the page root: inside an
    // instance body `worker: worker` would resolve to the instance's OWN
    // property (a self-binding that reads undefined), so the rails take
    // this alias instead.
    readonly property var workerApi: worker

    ScrollView {
        id: inspectorScroll
        objectName: "themeEditorInspectorScroll"
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: 300
        contentWidth: availableWidth
        clip: true
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

        Column {
            id: inspectorContent
            width: inspectorScroll.availableWidth
            spacing: 8


            Text {
                text: themeEditor.selectedView === "" ? "SELECTED THEME — SONGS"
                                                      : "SELECTED THEME — BIBLE"
                color: "#bbbbbb"
                font.pixelSize: 11
                font.bold: true
            }
            Column {
                objectName: "themeEditorSongsSection"
                width: parent.width
                spacing: 8
                visible: themeEditor.selectedView === ""
                Text { text: "Name"; color: "#b0b0b0"; font.pixelSize: 11 }
                PanelField {
                    id: nameField
                    objectName: "themeEditorNameField"
                    width: parent.width
                    onEditingFinished: themeEditor.applyField("name", text.trim())
                }
                // The shared inspector rail for this theme (task CS.5):
                // colours, type, box, effects and movement, every row
                // reading and writing through the theme adapter. The
                // automation names this suite (and the operator's muscle
                // memory) rely on are assigned in Component.onCompleted —
                // the SAME identifiers as before the extraction, held by
                // the part that now owns each row.
                AppearanceInspector {
                    id: songsInspector
                    width: parent.width
                    adapter: themeAdapter
                    worker: themeEditor.workerApi
                    overlayParent: themeEditor
                }
            }

            Column {
                objectName: "themeEditorBibleSection"
                width: parent.width
                spacing: 8
                visible: themeEditor.selectedView === "scripture"
                Text { text: "Name"; color: "#b0b0b0"; font.pixelSize: 11 }
                PanelField {
                    // The same record the Songs view edits — a theme's identity
                    // belongs to the theme, not to one view — kept reachable
                    // from either view so the operator never loses the row.
                    objectName: "themeEditorNameFieldBible"
                    width: parent.width
                    onEditingFinished: themeEditor.applyField("name", text.trim())
                    onVisibleChanged: if (visible) text = themeEditor.rowFor(
                                              themeEditor.selectedId) !== null
                                      ? themeEditor.rowFor(themeEditor.selectedId).name : ""
                }
                // The scripture reading context (tasks TF.4, TV.1): the named
                // entry this theme's BIBLE VIEW is. Typography and a box,
                // nothing else — the colours stay the theme's and the line
                // count stays the measurement's. An absent view is a real
                // state: the passage simply wears the Songs look.
                CheckBox {
                    id: ctxToggle
                    objectName: "themeEditorContextToggle"
                    text: "set passages apart"
                    font.pixelSize: 11
                    enabled: themeEditor.selectedId !== ""
                    checked: false
                    onToggled: themeEditor.setContext(checked)
                }
                Column {
                    objectName: "themeEditorContextFields"
                    width: parent.width
                    spacing: 8
                    visible: themeEditor.contextOn
                    // The shared inspector rail for the VIEW (task CS.5):
                    // sparse type rows, ONE box (with the theme's own
                    // rectangle borrowable), and the view's own movement.
                    // No colours and no effects — a text context cannot
                    // store them until TV.3, and the section is simply
                    // absent rather than a rail of disabled rows lying
                    // about what a Bible edit can do.
                    AppearanceInspector {
                        id: contextInspector
                        width: parent.width
                        adapter: contextAdapter
                        worker: themeEditor.workerApi
                        overlayParent: themeEditor
                        showBackground: false
                        showEffects: false
                    }
                    // The measured lines the passage's box holds
                    // (information, never a setting) — page-owned because
                    // the probe is the worker's, not any field's.
                    Text {
                        id: ctxLinesLabel
                        objectName: "themeEditorContextLines"
                        width: parent.width
                        wrapMode: Text.WordWrap
                        color: "#8ec9f5"
                        font.pixelSize: 11
                        text: ""
                    }
                    Text {
                        width: parent.width
                        wrapMode: Text.WordWrap
                        color: "#888888"
                        font.pixelSize: 10
                        text: "the Bible view sets type and a box — never a colour, "
                              + "and never a line count: how many lines ride a slide is "
                              + "what the renderer measures"
                    }
                }
            }
        }
    }

}
