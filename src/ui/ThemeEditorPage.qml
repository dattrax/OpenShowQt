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
        colourField.text = row !== null ? row.textColor : ""
        backField.text = row !== null ? row.backgroundColor : ""
        gradientField.text = row !== null ? row.backgroundGradient : ""
        fontField.text = row !== null ? row.fontFamily : ""
        sizeField.text = row !== null ? String(row.fontSize) : ""
        alignCombo.currentIndex = row !== null
                ? ["left", "center", "right"].indexOf(row.align) : 1
        spacingField.text = row !== null ? String(row.lineSpacing) : ""
        // The placement fields follow the box the overlay shows (the theme's
        // own, or the built-in lyric box it is being offered).
        const box = themeEditor.boxFor(row)
        placeXField.text = String(Math.round(box.x))
        placeYField.text = String(Math.round(box.y))
        placeWField.text = String(Math.round(box.width))
        placeHField.text = String(Math.round(box.height))
        themeEditor.refreshViewLook()
        // The style controls read the RESOLVED look, so they sync after it
        // is refreshed — one source of truth, the compiled sample.
        themeEditor.syncStyleControls(row)
        themeEditor.syncTransition(row)
        themeEditor.syncContext()
    }
    // Put the typography/effect controls where the record (or the resolved
    // look, where the theme is silent) says they belong. Assignment, not
    // binding, for the same reason as every other field here: a control the
    // operator is mid-drag on must not be yanked, and a successful commit
    // re-reads through documentsChanged anyway. The row comes in from
    // syncInspector's own rowFor, for the reason that function states: read
    // from inside the documentsChanged emit, the `row` property can still
    // hold the numbers the write just replaced, and a control repainted
    // from them would be a control showing a value the model does not have.
    function syncStyleControls(rowIn) {
        const row = rowIn !== undefined ? rowIn : themeEditor.row
        if (row === null)
            return
        const weights = [100, 200, 300, 400, 500, 600, 700, 800, 900]
        const w = weights.indexOf(Number(themeEditor.styleValue("fontWeight", "fontWeightSet", row)))
        weightCombo.currentIndex = w >= 0 ? w : 3
        slantCombo.currentIndex =
                themeEditor.styleValue("fontStyle", "fontStyleSet", row) === "italic" ? 1 : 0
        fitCombo.currentIndex =
                themeEditor.styleValue("fitMode", "fitModeSet", row) === "fixed" ? 1 : 0
        minSizeField.text = String(themeEditor.styleValue("minFontSize", "minFontSizeSet", row))
        const s = themeEditor.strokeShown(row)
        strokeEnable.checked = s.enabled === true
        strokeColorField.text = s.color !== undefined ? s.color : ""
        strokeWidthSlider.value = s.width !== undefined ? s.width : 0
        const sh = themeEditor.shadowShown(row)
        shadowEnable.checked = sh.enabled === true
        shadowColorField.text = sh.color !== undefined ? sh.color : ""
        shadowOpacitySlider.value = sh.opacity !== undefined ? sh.opacity : 0
        shadowOffsetXSlider.value = sh.offsetX !== undefined ? sh.offsetX : 0
        shadowOffsetYSlider.value = sh.offsetY !== undefined ? sh.offsetY : 0
        shadowBlurSlider.value = sh.blurSigma !== undefined ? sh.blurSigma : 0
    }
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

    /* --- typography, effects and the family picker (task TS.4) ------------
     * The style block is SHARED, so these controls use the ONE-FIELD merge
     * rather than re-spelling the whole record every keystroke: dragging
     * the blur slider lands ONE undo (commit on release, not per pixel),
     * and a field the operator tabs through without changing writes
     * NOTHING. A refused write snaps the control back to the record — a
     * field showing a number the model turned down is a field about to be
     * trusted. (The plan routes these to the theme while TV.3 has not
     * promoted the reading context to a view of its own; the scripture
     * context carries no style fields until then.)
     *
     * The controls show the theme's OWN value when it carries one, and the
     * value the audience actually draws today otherwise — read through the
     * sample's resolved appearance — with a badge naming which. The family
     * field stays a free-typed field always: it worked before the worker
     * could answer and still works disconnected. The Browse popup is the
     * one thing that needs the worker, and only a worker that named
     * font.list can fill it; a requested family the worker cannot resolve
     * is flagged MISSING but the stored name is never rewritten, because
     * the scene must keep what the author asked for, not the accident of
     * which worker was connected when it was typed.
     */
    function applyStyleField(field, value) {
        // A failure has no documentsChanged to repaint from, so re-read the
        // record here; a success repaints through the model's own emit.
        if (!docs.setThemeTextStyleField(themeEditor.selectedId, field, value))
            themeEditor.syncInspector()
    }
    // A slider drag is ONE gesture, so it commits once — on release — and
    // not at all when the handle ended where it began (the current value is
    // read before the write, so a no-move click writes nothing and costs no
    // undo, exactly like tabbing through a text field unchanged).
    function commitSlider(field, value, current) {
        if (Math.abs(Number(value) - Number(current)) < 1e-6)
            return
        themeEditor.applyStyleField(field, value)
    }
    // The number a control shows and the number it must compare a commit
    // against: the theme's own when set, the resolved answer otherwise. The
    // optional row is the caller's fresh read (syncInspector's); a binding
    // leaves it out and takes the property, which is current by then.
    function styleValue(ownKey, setKey, rowIn) {
        const row = rowIn !== undefined ? rowIn : themeEditor.row
        if (row === null)
            return null
        if (row[setKey] === true)
            return row[ownKey]
        const look = themeEditor.viewLook
        return look !== null && look.ok === true ? look[ownKey] : row[ownKey]
    }
    function styleInherited(setKey) {
        const row = themeEditor.row
        return row === null || row[setKey] !== true
    }
    // The effect a row of members edits: the theme's own record when it
    // carries one, the resolved one otherwise (silence has no record — the
    // badge says so and the members preview what will draw).
    function strokeShown(rowIn) {
        const row = rowIn !== undefined ? rowIn : themeEditor.row
        if (row === null)
            return {}
        if (row.strokeSet === true)
            return row.stroke
        const look = themeEditor.viewLook
        return look !== null && look.ok === true ? look.stroke : {}
    }
    function shadowShown(rowIn) {
        const row = rowIn !== undefined ? rowIn : themeEditor.row
        if (row === null)
            return {}
        if (row.shadowSet === true)
            return row.shadow
        const look = themeEditor.viewLook
        return look !== null && look.ok === true ? look.shadow : {}
    }
    // The family list narrowed by the search box. Re-derived from BOTH the
    // query text and the worker's arriving pages, so a search typed while
    // the walk is mid-page completes against the full list as it lands.
    function fontMatches(query) {
        const needle = query.trim().toLowerCase()
        return worker.fontFamilies.filter(f => needle === ""
                                               || f.toLowerCase().indexOf(needle) >= 0)
    }
    // "Can THIS worker draw the family the theme asks for?" Only a COMPLETED
    // list can answer, and only the WORKER'S list counts: absent means
    // missing; no list, an in-flight walk, or a worker that cannot list all
    // say nothing (they are not a verdict on the name).
    function fontMissing(row) {
        if (row === null || row.fontFamily === "" || !worker.fontListSupported
                || worker.fontListState !== "ready")
            return false
        const want = row.fontFamily.toLowerCase()
        return !worker.fontFamilies.some(f => f.toLowerCase() === want)
    }

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
    // Re-read the whole section from the record — the commit, the More
    // menu's undo, a refused write: whatever moved, the fields follow.
    function syncContext() {
        const row = themeEditor.themeRow()
        const ctx = themeEditor.contextRecord()
        themeEditor.contextOn = ctx.set === true
        ctxToggle.checked = ctx.set === true
        ctxFontField.text = ctx.fontFamilySet === true ? ctx.fontFamily : ""
        ctxSizeField.text = ctx.fontSizeSet === true ? String(ctx.fontSize) : ""
        ctxSpacingField.text = ctx.lineSpacingSet === true ? String(ctx.lineSpacing) : ""
        ctxAlignCombo.currentIndex = ctx.alignSet === true
                ? ["left", "center", "right"].indexOf(ctx.align) + 1 : 0
        themeEditor.contextPlaces = ctx.boxSet === true || ctx.insetSet === true
        // Silence SHOWS the box the passage would land on anyway — the
        // theme's lyric rectangle, or the built-in one — because that is
        // the honest answer to "where do the words go right now"; typing
        // over it states the context's own opinion.
        const box = ctx.boxSet === true
                ? Qt.rect(ctx.x, ctx.y, ctx.width, ctx.height) : themeEditor.boxFor(row)
        ctxXField.text = String(Math.round(box.x))
        ctxYField.text = String(Math.round(box.y))
        ctxWField.text = String(Math.round(box.width))
        ctxHField.text = String(Math.round(box.height))
        // The view's own move (task TR.3), synced with the rest of the entry:
        // the picker on the seat that answers, the numbers on what that seat
        // says, and the label naming it.
        const move = themeEditor.contextTransitionShown(row, ctx)
        contextTransitionCombo.currentIndex = ctx.transitionSet === true
                ? (ctx.transition.kind === "cut" ? 1 : 2) : 0
        contextTransitionWordsField.text = String(move.textDurationMs)
        contextTransitionBackField.text = String(move.mediaDurationMs)
        themeEditor.contextTransition = move
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
    // A field's commit is a WHOLE-entry write: the current set merged with
    // the one field, because the domain, like the file, replaces the entry
    // it is given. null is the field's way of saying "be silent again".
    // The one place-task rule (task TV.1): the view answers with ONE box —
    // writing its own rectangle retires a stored inset in the SAME edit,
    // and "No box" clears the box and inset keys together while the entry's
    // typography stays. Removing the whole VIEW is the toggle's job, not a
    // field's.
    function commitContext(overrides) {
        const id = themeEditor.selectedId
        const current = themeEditor.contextRecord()
        if (id === "" || current.set !== true)
            return
        const fields = {}
        if (current.fontFamilySet === true)
            fields.fontFamily = current.fontFamily
        if (current.fontSizeSet === true)
            fields.fontSize = current.fontSize
        if (current.lineSpacingSet === true)
            fields.lineSpacing = current.lineSpacing
        if (current.alignSet === true)
            fields.align = current.align
        if (current.insetSet === true && overrides.box === undefined)
            fields.inset = current.inset
        if (current.boxSet === true && overrides.inset === undefined)
            fields.box = {x: current.x, y: current.y, width: current.width,
                          height: current.height}
        for (const key in overrides) {
            if (overrides[key] === null)
                delete fields[key]
            else
                fields[key] = overrides[key]
        }
        if (!docs.applyThemeContext(id, "scripture", fields))
            themeEditor.syncContext()  // the refusal wins: the fields go back
    }
    // Four fields commit together, exactly like the theme's own placement:
    // a box is one unit, and a half-typed one is a wrong one.
    function applyContextBox() {
        const x = parseFloat(ctxXField.text)
        const y = parseFloat(ctxYField.text)
        const w = parseFloat(ctxWField.text)
        const h = parseFloat(ctxHField.text)
        if (!isFinite(x) || !isFinite(y) || !isFinite(w) || !isFinite(h) || w <= 0 || h <= 0) {
            themeEditor.syncContext()
            return
        }
        themeEditor.commitContext({box: {x: x, y: y, width: w, height: h}})
    }

    /* --- how a view moves (task TR.3) --------------------------------------
     * A transition is a property of how a VIEW presents, so the Songs section
     * edits the theme's own record and the Bible section the `scripture`
     * context's — the two seats TR.1's resolver walks, in the order it walks
     * them: the context first, then the theme that carries it, then whatever
     * else the chain speaks with, then the service. This page READS those
     * records rather than re-walking a chain of its own — a second walk is a
     * second opinion about priority, and the one door is
     * DocumentController::transitionForSlide, which the cue lane asks the same
     * question at Go. So the numbers a silent view shows are the numbers the
     * seat below it answers with today.
     *
     * Three choices, no more. Inherit writes NOTHING at all — the record is
     * removed, which is a different act from writing a record that happens to
     * match what the view would inherit (and the reason the Inherit button is
     * enabled only when there is a record to take off). Cut and Crossfade each
     * write a WHOLE record, kind plus both durations, because a transition is
     * atomic at the file, on the wire and in the domain, and this is not the
     * place that gets loosened.
     *
     * A Cut disables the two durations without erasing them: the record still
     * holds them, so coming back to Crossfade comes back to the times that
     * were there. An edit here changes the NEXT cue wearing this theme — the
     * record a cue moves with is captured when Go is pressed, so a fade
     * already crossing finishes at the lengths it was given. And none of this
     * can present anything: the theme under edit may not be on air at all, and
     * the picture in the canvas is a fixture the audience never sees.
     */
    // The answer the view under edit shows: which seat speaks, and with what.
    property var themeTransition: ({own: false, kind: "fade", textDurationMs: 500,
                                    mediaDurationMs: 800, seat: "the service"})
    property var contextTransition: ({own: false, kind: "fade", textDurationMs: 500,
                                      mediaDurationMs: 800, seat: "the service"})
    // The Inherit button's honest enable: there is a record to take off only
    // when this view is the seat that speaks. (A button that wrote "nothing"
    // over nothing would cost an undo for no change.)
    readonly property bool contextTransitionOwn: contextTransition.own === true
    // The service's standing record, for the note under a view that says
    // nothing: the seat at the bottom of every chain.
    function serviceTransition() {
        return docs.serviceDefaultTransition()
    }
    // The SONGS view: the theme's own record when it carries one, the
    // service's otherwise. A theme that says nothing defers to the chain a
    // slide wearing it builds — lower themes answer before the service does —
    // and the note under the control says so rather than pretending the
    // service is the only answer beneath it.
    function themeTransitionShown(row) {
        if (row !== null && row.transitionSet === true) {
            return {own: true, kind: row.transition.kind,
                    textDurationMs: row.transition.textDurationMs,
                    mediaDurationMs: row.transition.mediaDurationMs, seat: "this theme"}
        }
        const service = themeEditor.serviceTransition()
        return {own: false, kind: service.kind, textDurationMs: service.textDurationMs,
                mediaDurationMs: service.mediaDurationMs, seat: "the service"}
    }
    // The BIBLE view: its own context, then the theme carrying it, then the
    // service — the same priority, read from the two records in front of the
    // page instead of from a walk this page could get wrong.
    function contextTransitionShown(row, ctx) {
        if (ctx.transitionSet === true) {
            return {own: true, kind: ctx.transition.kind,
                    textDurationMs: ctx.transition.textDurationMs,
                    mediaDurationMs: ctx.transition.mediaDurationMs, seat: "this view"}
        }
        if (row !== null && row.transitionSet === true) {
            return {own: false, kind: row.transition.kind,
                    textDurationMs: row.transition.textDurationMs,
                    mediaDurationMs: row.transition.mediaDurationMs, seat: "this theme"}
        }
        const service = themeEditor.serviceTransition()
        return {own: false, kind: service.kind, textDurationMs: service.textDurationMs,
                mediaDurationMs: service.mediaDurationMs, seat: "the service"}
    }
    // The sentence the source label shows: who answers, and with what numbers.
    // A Cut is said on its own terms — quoting times it ignores as if they
    // applied would be the page lying about the move the audience gets.
    function describeTransition(shown) {
        const who = shown.own === true ? shown.seat + " answers for itself"
                                       : "inherits " + shown.seat
        if (shown.kind === "cut")
            return who + " — Cut: the cue is simply on"
        return who + " — Crossfade " + shown.textDurationMs + " ms words / "
               + shown.mediaDurationMs + " ms background"
    }
    // A typed millisecond count, or -1 for "that is not a duration" — shape
    // only. The BAND is the domain's one sentence: a 99999 that gets here is
    // the verb's to refuse, and the refusal re-reads the record, so the
    // control snaps back to the model instead of to a ceiling copied twice.
    function msOr(textIn) {
        const t = String(textIn).trim()
        if (t === "" || !/^[0-9]+$/.test(t))
            return -1
        return Math.round(Number(t))
    }
    // The two writes, each ONE record and therefore one undo. A refusal has no
    // documentsChanged to repaint from, so the section re-reads itself; a
    // success repaints through the model's own emit.
    function writeThemeTransition(record) {
        if (!docs.setThemeTransition(themeEditor.selectedId, record))
            themeEditor.syncInspector()
    }
    function writeContextTransition(record) {
        if (!docs.setTextContextTransition(themeEditor.selectedId, "scripture", record))
            themeEditor.syncContext()
    }
    // Inherit is the removal, and only ever offered against a record that
    // exists: pressing it when the view already says nothing writes nothing and
    // would cost an undo for no change.
    function inheritThemeTransition() {
        const row = themeEditor.themeRow()
        if (row !== null && row.transitionSet === true)
            themeEditor.writeThemeTransition({})
    }
    function inheritContextTransition() {
        if (themeEditor.contextRecord().transitionSet === true)
            themeEditor.writeContextTransition({})
    }
    // The SONGS commit: the kind the picker shows plus the two numbers the
    // fields show. Inherit is the picker's first entry, and a number that is
    // not a count of milliseconds is refused by the page rather than turned
    // into one — 0 would be a duration the operator did not type.
    function commitThemeTransitionView() {
        if (themeEditor.selectedId === "")
            return
        if (transitionCombo.currentIndex === 0) {
            themeEditor.inheritThemeTransition()
            return
        }
        const kind = transitionCombo.currentIndex === 1 ? "cut" : "fade"
        const words = themeEditor.msOr(transitionWordsField.text)
        const back = themeEditor.msOr(transitionBackField.text)
        if (words < 0 || back < 0) {
            themeEditor.syncInspector()
            return
        }
        const shown = themeEditor.themeTransitionShown(themeEditor.themeRow())
        if (shown.own === true && shown.kind === kind
                && shown.textDurationMs === words && shown.mediaDurationMs === back)
            return  // focus moved with nothing changed: no write, no undo
        themeEditor.writeThemeTransition({kind: kind, textDurationMs: words,
                                          mediaDurationMs: back})
    }
    // The BIBLE commit, same shape, and the same refusal to write anything a
    // context that does not exist could not hold.
    function commitContextTransitionView() {
        const ctx = themeEditor.contextRecord()
        if (themeEditor.selectedId === "" || ctx.set !== true)
            return
        if (contextTransitionCombo.currentIndex === 0) {
            themeEditor.inheritContextTransition()
            return
        }
        const kind = contextTransitionCombo.currentIndex === 1 ? "cut" : "fade"
        const words = themeEditor.msOr(contextTransitionWordsField.text)
        const back = themeEditor.msOr(contextTransitionBackField.text)
        if (words < 0 || back < 0) {
            themeEditor.syncContext()
            return
        }
        const shown = themeEditor.contextTransitionShown(themeEditor.themeRow(), ctx)
        if (shown.own === true && shown.kind === kind
                && shown.textDurationMs === words && shown.mediaDurationMs === back)
            return
        themeEditor.writeContextTransition({kind: kind, textDurationMs: words,
                                            mediaDurationMs: back})
    }
    // Put the Songs section's three controls where the record says they
    // belong. Assignment, not binding, like every other field here: a control
    // the operator is mid-edit on must not be yanked from under them.
    function syncTransition(rowIn) {
        const row = rowIn !== undefined ? rowIn : themeEditor.row
        const shown = themeEditor.themeTransitionShown(row)
        transitionCombo.currentIndex = row !== null && row.transitionSet === true
                ? (row.transition.kind === "cut" ? 1 : 2) : 0
        transitionWordsField.text = String(shown.textDurationMs)
        transitionBackField.text = String(shown.mediaDurationMs)
        themeEditor.themeTransition = shown
    }

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
    // (retiring any inset in the same edit, per commitContext), and the
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
            themeEditor.commitContext({box: {
                x: Math.round(themeEditor.penX), y: Math.round(themeEditor.penY),
                width: Math.round(themeEditor.penW), height: Math.round(themeEditor.penH)}})
        } else {
            docs.setThemePlacement(id, Math.round(themeEditor.penX), Math.round(themeEditor.penY),
                                   Math.round(themeEditor.penW), Math.round(themeEditor.penH))
        }
        themeEditor.syncInspector()
    }
    // The exact-number path. Four fields commit together because a placement
    // is one unit; anything the model refuses (or that is not a number) puts
    // the fields back where the model says they belong.
    function applyPlacementFields() {
        const row = themeEditor.row
        const x = parseFloat(placeXField.text)
        const y = parseFloat(placeYField.text)
        const w = parseFloat(placeWField.text)
        const h = parseFloat(placeHField.text)
        if (row === null || !Number.isFinite(x) || !Number.isFinite(y)
                || !(w >= 1) || !(h >= 1)
                || !docs.setThemePlacement(row.id, x, y, w, h))
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
            Rectangle {
                width: tileButton.picWidth
                height: tileButton.picWidth * 9 / 16
                color: "#000000"
                border.width: 1
                border.color: themeEditor.selectedView === tileButton.view
                              ? "#1685f8" : "#4a4a4a"
                // The worker's own picture of THIS view's fixture — the
                // same key the batch asked with. With no lane open the
                // tile is an honest black frame and a glyph, never a QML
                // guess at the theme's look.
                PreviewView {
                    anchors.fill: parent
                    visible: worker.thumbnailSupported
                    controller: worker
                    target: PreviewView.Thumbnail
                    key: themeEditor.selectedId === ""
                         ? "" : themeEditor.viewThumbKey(tileButton.view)
                }
                Text {
                    anchors.centerIn: parent
                    visible: !worker.thumbnailSupported
                    color: "#888888"
                    font.pixelSize: 18
                    text: tileButton.view === "" ? "\u266a" : "\u271d"
                }
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

        Rectangle {
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
            color: "#000000"
            border.color: "#4a4a4a"
            clip: true
            PreviewView {
                anchors.fill: parent
                controller: worker
                target: PreviewView.Selection
                visible: worker.selectionReady
            }
            // No picture (no worker, the first ask still in flight): the
            // frame says which, rather than painting a QML guess at it.
            Text {
                anchors.centerIn: parent
                visible: !worker.selectionReady
                color: "#888888"
                font.pixelSize: 12
                text: themeEditor.selectedId === "" ? "No theme selected"
                      : worker.statusNeedsAttention ? "Preview unavailable"
                                                    : "Waiting for the preview"
            }

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
                Text { text: "Text colour (#rgb or #rrggbb)"; color: "#b0b0b0"; font.pixelSize: 11 }
                PanelField {
                    id: colourField
                    objectName: "themeEditorColourField"
                    width: parent.width
                    onEditingFinished: themeEditor.applyField("textColor", text)
                }
                Text { text: "Background colour"; color: "#b0b0b0"; font.pixelSize: 11 }
                PanelField {
                    id: backField
                    objectName: "themeEditorBackField"
                    width: parent.width
                    onEditingFinished: themeEditor.applyField("backgroundColor", text)
                }
                Text { text: "Gradient colour (\"\" = flat)"; color: "#b0b0b0"; font.pixelSize: 11 }
                PanelField {
                    id: gradientField
                    objectName: "themeEditorGradientField"
                    width: parent.width
                    onEditingFinished: themeEditor.applyField("backgroundGradient", text)
                }
                Text { text: "Font family (\"\" = platform)"; color: "#b0b0b0"; font.pixelSize: 11 }
                // The typed name is the real control: it always worked and
                // works with the worker gone. "Browse" only opens a list the
                // audience's OWN worker can honour; a worker that named no
                // font.list gets a disabled button and a word why, never a
                // list of the operator's desktop fonts the show would fall
                // back from (task TS.4).
                Row {
                    spacing: 4
                    PanelField {
                        id: fontField
                        objectName: "themeEditorFontField"
                        width: parent.width - browseFontsButton.width - parent.spacing
                        onEditingFinished: themeEditor.applyField("fontFamily", text)
                    }
                    PanelButton {
                        id: browseFontsButton
                        objectName: "themeEditorBrowseFonts"
                        text: "Browse"
                        enabled: worker.fontListSupported && themeEditor.selectedId !== ""
                        onClicked: {
                            fontSearchField.text = ""
                            if (worker.fontListState === "idle")
                                worker.refreshFontFamilies()
                            fontPopup.open()
                        }
                    }
                }
                Text {
                    objectName: "themeEditorFontStatus"
                    width: parent.width
                    wrapMode: Text.WordWrap
                    font.pixelSize: 10
                    color: themeEditor.fontMissing(themeEditor.row) ? "#e0a94a" : "#888888"
                    text: {
                        const row = themeEditor.row
                        if (row === null || row.fontFamily === "")
                            return ""
                        if (!worker.fontListSupported)
                            return "typed name — this worker cannot list its fonts"
                        if (worker.fontListState !== "ready")
                            return "checking the audience's fonts…"
                        return themeEditor.fontMissing(row)
                                ? "\"" + row.fontFamily + "\" is not installed on the "
                                  + "audience's machine — it will fall back"
                                : "installed on the audience's machine"
                    }
                }
                Text { text: "Font size (0 = renderer default)"; color: "#b0b0b0"; font.pixelSize: 11 }
                PanelField {
                    id: sizeField
                    objectName: "themeEditorSizeField"
                    width: parent.width
                    onEditingFinished: themeEditor.applyField("fontSize", Number(text))
                }
                Text { text: "Alignment"; color: "#b0b0b0"; font.pixelSize: 11 }
                ComboBox {
                    id: alignCombo
                    objectName: "themeEditorAlign"
                    width: parent.width
                    model: ["left", "center", "right"]
                    onActivated: themeEditor.applyField("align", currentValue)
                }
                Text { text: "Line spacing (0.25 to 8)"; color: "#b0b0b0"; font.pixelSize: 11 }
                PanelField {
                    id: spacingField
                    objectName: "themeEditorSpacingField"
                    width: parent.width
                    onEditingFinished: themeEditor.applyField("lineSpacing", Number(text))
                }

                // The lyric box this theme places (themes plan amendment
                // 2026-11-01): the same rectangle the overlay above is showing,
                // in the 1920x1080 authored canvas, for the operator who would
                // rather type it. Four fields commit together — a placement is
                // ONE unit — and "No placement" takes the theme's opinion away
                // so wearing slides draw the boxes they were authored with.
                Text {
                    text: "Lyric box (x, y, w, h in 1920×1080)"
                    color: "#b0b0b0"
                    font.pixelSize: 11
                }
                Row {
                    spacing: 4
                    PanelField {
                        id: placeXField
                        objectName: "themeEditorPlaceX"
                        width: 52
                        onEditingFinished: themeEditor.applyPlacementFields()
                    }
                    PanelField {
                        id: placeYField
                        objectName: "themeEditorPlaceY"
                        width: 52
                        onEditingFinished: themeEditor.applyPlacementFields()
                    }
                    PanelField {
                        id: placeWField
                        objectName: "themeEditorPlaceW"
                        width: 52
                        onEditingFinished: themeEditor.applyPlacementFields()
                    }
                    PanelField {
                        id: placeHField
                        objectName: "themeEditorPlaceH"
                        width: 52
                        onEditingFinished: themeEditor.applyPlacementFields()
                    }
                }
                Row {
                    spacing: 6
                    PanelButton {
                        objectName: "themeEditorNoPlacement"
                        text: "No placement"
                        enabled: themeEditor.hasPlacement
                        onClicked: docs.clearThemePlacement(themeEditor.selectedId)
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        color: themeEditor.hasPlacement ? "#8ec9f5" : "#888888"
                        font.pixelSize: 11
                        text: themeEditor.hasPlacement ? "this theme places its slides"
                                                       : "no opinion — slides keep their own boxes"
                    }
                }

                // --- typography & effects (task TS.4) ---------------------
                // Each control commits ONE field through the merge verb, so
                // a change here never disturbs the others and never costs
                // more than one undo. The badge beside each name says
                // whether this theme speaks or inherits the value the
                // audience draws today; "Inherit"/"Off" buttons write that
                // back (a scalar to null, an effect cleared whole). Sliders
                // commit on release only — a drag is one gesture is one
                // undo. A refused write re-reads the record (applyStyleField).
                Text {
                    text: "Typography & effects"
                    color: "#bbbbbb"
                    font.pixelSize: 11
                    font.bold: true
                }
                Row {
                    spacing: 6
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: "Weight"; color: "#b0b0b0"; font.pixelSize: 11
                    }
                    ComboBox {
                        id: weightCombo
                        objectName: "themeEditorWeight"
                        model: [100, 200, 300, 400, 500, 600, 700, 800, 900]
                        onActivated: themeEditor.applyStyleField("fontWeight",
                                                                 Number(currentValue))
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        font.pixelSize: 10
                        color: themeEditor.styleInherited("fontWeightSet") ? "#888888" : "#8ec9f5"
                        text: themeEditor.styleInherited("fontWeightSet") ? "inherited" : "own"
                    }
                    PanelButton {
                        objectName: "themeEditorWeightInherit"
                        text: "Inherit"
                        enabled: !themeEditor.styleInherited("fontWeightSet")
                        onClicked: themeEditor.applyStyleField("fontWeight", null)
                    }
                }
                Row {
                    spacing: 6
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: "Slant"; color: "#b0b0b0"; font.pixelSize: 11
                    }
                    ComboBox {
                        id: slantCombo
                        objectName: "themeEditorSlant"
                        model: ["normal", "italic"]
                        onActivated: themeEditor.applyStyleField("fontStyle", currentValue)
                    }
                    PanelButton {
                        objectName: "themeEditorSlantInherit"
                        text: "Inherit"
                        enabled: !themeEditor.styleInherited("fontStyleSet")
                        onClicked: themeEditor.applyStyleField("fontStyle", null)
                    }
                }
                Row {
                    spacing: 6
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: "Fit"; color: "#b0b0b0"; font.pixelSize: 11
                    }
                    ComboBox {
                        id: fitCombo
                        objectName: "themeEditorFit"
                        model: ["shrink", "fixed"]
                        onActivated: themeEditor.applyStyleField("fitMode", currentValue)
                    }
                    PanelButton {
                        objectName: "themeEditorFitInherit"
                        text: "Inherit"
                        enabled: !themeEditor.styleInherited("fitModeSet")
                        onClicked: themeEditor.applyStyleField("fitMode", null)
                    }
                }
                Text { text: "Minimum size (1..4096)"; color: "#b0b0b0"; font.pixelSize: 11 }
                Row {
                    spacing: 4
                    PanelField {
                        id: minSizeField
                        objectName: "themeEditorMinSize"
                        width: parent.width - parent.spacing - 60
                        onEditingFinished: {
                            if (text.trim() === String(themeEditor.styleValue("minFontSize",
                                                                              "minFontSizeSet")))
                                return  // focus-out with no change writes nothing
                            const v = Number(text)
                            if (isFinite(v) && v >= 1 && v <= 4096)
                                themeEditor.applyStyleField("minFontSize", v)
                            else
                                themeEditor.syncStyleControls()
                        }
                    }
                    PanelButton {
                        objectName: "themeEditorMinSizeInherit"
                        text: "Inherit"
                        width: 60
                        enabled: !themeEditor.styleInherited("minFontSizeSet")
                        onClicked: themeEditor.applyStyleField("minFontSize", null)
                    }
                }

                // Stroke: enable is the record's own materialise — a theme
                // that carried nothing gains {true, black, 2} (the audience
                // already draws that as the default); members then merge.
                Row {
                    spacing: 6
                    CheckBox {
                        id: strokeEnable
                        objectName: "themeEditorStrokeEnable"
                        text: "outline"; font.pixelSize: 11
                        onToggled: themeEditor.applyStyleField("strokeEnabled", checked)
                    }
                    PanelButton {
                        objectName: "themeEditorStrokeInherit"
                        text: "Off"
                        enabled: themeEditor.row !== null && themeEditor.row.strokeSet === true
                        onClicked: themeEditor.applyStyleField("stroke", null)
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        font.pixelSize: 10
                        color: themeEditor.styleInherited("strokeSet") ? "#888888" : "#8ec9f5"
                        text: themeEditor.styleInherited("strokeSet") ? "inherited" : "own"
                    }
                }
                Row {
                    spacing: 4
                    Text { text: "colour"; color: "#b0b0b0"; font.pixelSize: 11 }
                    PanelField {
                        id: strokeColorField
                        objectName: "themeEditorStrokeColour"
                        width: 110
                        onEditingFinished: {
                            if (text.trim() === String(themeEditor.strokeShown().color))
                                return
                            themeEditor.applyStyleField("strokeColor", text.trim())
                        }
                    }
                }
                Row {
                    spacing: 4
                    Text { text: "width"; color: "#b0b0b0"; font.pixelSize: 11 }
                    Slider {
                        id: strokeWidthSlider
                        objectName: "themeEditorStrokeWidth"
                        from: 0; to: 32; stepSize: 0.5
                        onPressedChanged: if (!pressed)
                            themeEditor.commitSlider("strokeWidth", value,
                                                     themeEditor.strokeShown().width)
                    }
                }

                // Shadow: same shape — enable materialises the default
                // record, the four sliders merge one member on release.
                Row {
                    spacing: 6
                    CheckBox {
                        id: shadowEnable
                        objectName: "themeEditorShadowEnable"
                        text: "shadow"; font.pixelSize: 11
                        onToggled: themeEditor.applyStyleField("shadowEnabled", checked)
                    }
                    PanelButton {
                        objectName: "themeEditorShadowInherit"
                        text: "Off"
                        enabled: themeEditor.row !== null && themeEditor.row.shadowSet === true
                        onClicked: themeEditor.applyStyleField("shadow", null)
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        font.pixelSize: 10
                        color: themeEditor.styleInherited("shadowSet") ? "#888888" : "#8ec9f5"
                        text: themeEditor.styleInherited("shadowSet") ? "inherited" : "own"
                    }
                }
                Row {
                    spacing: 4
                    Text { text: "colour"; color: "#b0b0b0"; font.pixelSize: 11 }
                    PanelField {
                        id: shadowColorField
                        objectName: "themeEditorShadowColour"
                        width: 110
                        onEditingFinished: {
                            if (text.trim() === String(themeEditor.shadowShown().color))
                                return
                            themeEditor.applyStyleField("shadowColor", text.trim())
                        }
                    }
                }
                Row {
                    spacing: 4
                    Text { text: "opacity"; color: "#b0b0b0"; font.pixelSize: 11 }
                    Slider {
                        id: shadowOpacitySlider
                        objectName: "themeEditorShadowOpacity"
                        from: 0; to: 1; stepSize: 0.05
                        onPressedChanged: if (!pressed)
                            themeEditor.commitSlider("shadowOpacity", value,
                                                     themeEditor.shadowShown().opacity)
                    }
                }
                Row {
                    spacing: 4
                    Text { text: "offset X"; color: "#b0b0b0"; font.pixelSize: 11 }
                    Slider {
                        id: shadowOffsetXSlider
                        objectName: "themeEditorShadowOffsetX"
                        from: -256; to: 256; stepSize: 1
                        onPressedChanged: if (!pressed)
                            themeEditor.commitSlider("shadowOffsetX", value,
                                                     themeEditor.shadowShown().offsetX)
                    }
                }
                Row {
                    spacing: 4
                    Text { text: "offset Y"; color: "#b0b0b0"; font.pixelSize: 11 }
                    Slider {
                        id: shadowOffsetYSlider
                        objectName: "themeEditorShadowOffsetY"
                        from: -256; to: 256; stepSize: 1
                        onPressedChanged: if (!pressed)
                            themeEditor.commitSlider("shadowOffsetY", value,
                                                     themeEditor.shadowShown().offsetY)
                    }
                }
                Row {
                    spacing: 4
                    Text { text: "blur σ"; color: "#b0b0b0"; font.pixelSize: 11 }
                    Slider {
                        id: shadowBlurSlider
                        objectName: "themeEditorShadowBlur"
                        from: 0; to: 64; stepSize: 0.5
                        onPressedChanged: if (!pressed)
                            themeEditor.commitSlider("shadowBlurSigma", value,
                                                     themeEditor.shadowShown().blurSigma)
                    }
                }

                // --- transition (task TR.3) -------------------------------
                // The view's own answer about how a wearing cue ARRIVES. The
                // picker's three entries are the whole contract: Inherit
                // removes the record, Cut and Crossfade each write one whole
                // record. A silent view shows the numbers the seat under it
                // answers with today, and the label under the fields says
                // which seat that is — the page reads the records rather than
                // re-walking the chain, so it cannot disagree with the
                // resolver a Go asks.
                Text {
                    text: "Transition"
                    color: "#bbbbbb"
                    font.pixelSize: 11
                    font.bold: true
                }
                ComboBox {
                    id: transitionCombo
                    objectName: "themeEditorTransitionKind"
                    width: parent.width
                    enabled: themeEditor.selectedId !== ""
                    model: ["Inherit", "Cut", "Crossfade"]
                    onActivated: themeEditor.commitThemeTransitionView()
                }
                Row {
                    spacing: 6
                    PanelButton {
                        objectName: "themeEditorTransitionInherit"
                        text: "Inherit"
                        enabled: themeEditor.row !== null
                                 && themeEditor.row.transitionSet === true
                        onClicked: themeEditor.inheritThemeTransition()
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        font.pixelSize: 10
                        color: themeEditor.themeTransition.own === true ? "#8ec9f5" : "#888888"
                        text: themeEditor.themeTransition.own === true ? "own" : "inherited"
                    }
                }
                Text { text: "Words duration (ms, 0 to 10000)"
                       color: "#b0b0b0"; font.pixelSize: 11 }
                PanelField {
                    id: transitionWordsField
                    objectName: "themeEditorTransitionWords"
                    width: parent.width
                    // A Cut ignores the numbers, and Inherit has none of its
                    // own to edit — both disable the fields WITHOUT clearing
                    // them, because the record still holds what is shown and
                    // coming back to Crossfade comes back to the times.
                    enabled: themeEditor.themeTransition.own === true
                             && themeEditor.themeTransition.kind === "fade"
                    onEditingFinished: themeEditor.commitThemeTransitionView()
                }
                Text { text: "Background duration (ms, 0 to 10000)"
                       color: "#b0b0b0"; font.pixelSize: 11 }
                PanelField {
                    id: transitionBackField
                    objectName: "themeEditorTransitionMedia"
                    width: parent.width
                    enabled: themeEditor.themeTransition.own === true
                             && themeEditor.themeTransition.kind === "fade"
                    onEditingFinished: themeEditor.commitThemeTransitionView()
                }
                Text {
                    objectName: "themeEditorTransitionSource"
                    width: parent.width
                    wrapMode: Text.WordWrap
                    font.pixelSize: 10
                    color: themeEditor.themeTransition.own === true ? "#8ec9f5" : "#888888"
                    text: themeEditor.describeTransition(themeEditor.themeTransition)
                          + (themeEditor.themeTransition.own === true
                             ? ""
                             : " — a theme lower in a wearing slide's chain answers first")
                }
                Text {
                    objectName: "themeEditorTransitionNote"
                    width: parent.width
                    wrapMode: Text.WordWrap
                    font.pixelSize: 10
                    color: "#888888"
                    text: "Words covers the text and any foreground picture; the background is "
                        + "a colour, a gradient or a still image. A background that is a VIDEO "
                        + "still cuts either way, so no background number dissolves one clip "
                        + "into the next. What is set here is the NEXT cue wearing this theme: "
                        + "a fade already crossing keeps the lengths it was given at Go."
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
                    Text { text: "Reading font (\"\" = theme's)"; color: "#b0b0b0"; font.pixelSize: 11 }
                    PanelField {
                        id: ctxFontField
                        objectName: "themeEditorContextFont"
                        width: parent.width
                        onEditingFinished: themeEditor.commitContext(
                                    {fontFamily: text.trim() === "" ? null : text.trim()})
                    }
                    Text { text: "Reading size (0 = renderer, \"\" = theme's)"
                               color: "#b0b0b0"; font.pixelSize: 11 }
                    PanelField {
                        id: ctxSizeField
                        objectName: "themeEditorContextSize"
                        width: parent.width
                        onEditingFinished: {
                            const t = text.trim()
                            if (t === "") {
                                themeEditor.commitContext({fontSize: null})
                                return
                            }
                            const v = Number(t)
                            if (!isFinite(v) || v < 0) {
                                themeEditor.syncContext()
                                return
                            }
                            themeEditor.commitContext({fontSize: v})
                        }
                    }
                    Text { text: "Reading alignment"; color: "#b0b0b0"; font.pixelSize: 11 }
                    ComboBox {
                        id: ctxAlignCombo
                        objectName: "themeEditorContextAlign"
                        width: parent.width
                        model: ["(theme)", "left", "center", "right"]
                        onActivated: themeEditor.commitContext(
                                    {align: currentIndex === 0 ? null : currentValue})
                    }
                    Text { text: "Reading line spacing (0.25 to 8, \"\" = theme's)"
                               color: "#b0b0b0"; font.pixelSize: 11 }
                    PanelField {
                        id: ctxSpacingField
                        objectName: "themeEditorContextSpacing"
                        width: parent.width
                        onEditingFinished: {
                            const t = text.trim()
                            if (t === "") {
                                themeEditor.commitContext({lineSpacing: null})
                                return
                            }
                            const v = Number(t)
                            if (!isFinite(v) || v < 0.25 || v > 8) {
                                themeEditor.syncContext()
                                return
                            }
                            themeEditor.commitContext({lineSpacing: v})
                        }
                    }
                    Text { text: "Bible box (x, y, w, h in 1920×1080)"
                               color: "#b0b0b0"; font.pixelSize: 11 }
                    Row {
                        spacing: 4
                        PanelField {
                            id: ctxXField
                            objectName: "themeEditorContextX"
                            width: 52
                            onEditingFinished: themeEditor.applyContextBox()
                        }
                        PanelField {
                            id: ctxYField
                            objectName: "themeEditorContextY"
                            width: 52
                            onEditingFinished: themeEditor.applyContextBox()
                        }
                        PanelField {
                            id: ctxWField
                            objectName: "themeEditorContextW"
                            width: 52
                            onEditingFinished: themeEditor.applyContextBox()
                        }
                        PanelField {
                            id: ctxHField
                            objectName: "themeEditorContextH"
                            width: 52
                            onEditingFinished: themeEditor.applyContextBox()
                        }
                    }
                    Row {
                        spacing: 6
                        PanelButton {
                            objectName: "themeEditorContextBoxTheme"
                            text: "Theme's lyric box"
                            onClicked: {
                                const box = themeEditor.boxFor(themeEditor.themeRow())
                                themeEditor.commitContext({box: {x: box.x, y: box.y,
                                                                 width: box.width, height: box.height}})
                            }
                        }
                        PanelButton {
                            objectName: "themeEditorContextBoxClear"
                            text: "No box"
                            onClicked: themeEditor.commitContext({box: null})
                        }
                    }
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

                    // The view's own move (task TR.3): the context's record,
                    // which a Go on a passage in this view reads BEFORE the
                    // theme that carries it — the nearer answer about how the
                    // view it is drawn in arrives. Same three choices, same
                    // atomic record, same disabled-not-erased numbers.
                    Text {
                        text: "Transition"
                        color: "#bbbbbb"
                        font.pixelSize: 11
                        font.bold: true
                    }
                    ComboBox {
                        id: contextTransitionCombo
                        objectName: "themeEditorContextTransitionKind"
                        width: parent.width
                        model: ["Inherit", "Cut", "Crossfade"]
                        onActivated: themeEditor.commitContextTransitionView()
                    }
                    Row {
                        spacing: 6
                        PanelButton {
                            objectName: "themeEditorContextTransitionInherit"
                            text: "Inherit"
                            enabled: themeEditor.contextTransitionOwn === true
                            onClicked: themeEditor.inheritContextTransition()
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            font.pixelSize: 10
                            color: themeEditor.contextTransition.own === true
                                     ? "#8ec9f5" : "#888888"
                            text: themeEditor.contextTransition.own === true ? "own"
                                                                              : "inherited"
                        }
                    }
                    Text { text: "Words duration (ms, 0 to 10000)"
                           color: "#b0b0b0"; font.pixelSize: 11 }
                    PanelField {
                        id: contextTransitionWordsField
                        objectName: "themeEditorContextTransitionWords"
                        width: parent.width
                        enabled: themeEditor.contextTransition.own === true
                                 && themeEditor.contextTransition.kind === "fade"
                        onEditingFinished: themeEditor.commitContextTransitionView()
                    }
                    Text { text: "Background duration (ms, 0 to 10000)"
                           color: "#b0b0b0"; font.pixelSize: 11 }
                    PanelField {
                        id: contextTransitionBackField
                        objectName: "themeEditorContextTransitionMedia"
                        width: parent.width
                        enabled: themeEditor.contextTransition.own === true
                                 && themeEditor.contextTransition.kind === "fade"
                        onEditingFinished: themeEditor.commitContextTransitionView()
                    }
                    Text {
                        objectName: "themeEditorContextTransitionSource"
                        width: parent.width
                        wrapMode: Text.WordWrap
                        font.pixelSize: 10
                        color: themeEditor.contextTransition.own === true ? "#8ec9f5" : "#888888"
                        text: themeEditor.describeTransition(themeEditor.contextTransition)
                              + (themeEditor.contextTransition.own === true
                                 ? ""
                                 : " — the theme carrying this view answers before the service")
                    }
                    Text {
                        objectName: "themeEditorContextTransitionNote"
                        width: parent.width
                        wrapMode: Text.WordWrap
                        font.pixelSize: 10
                        color: "#888888"
                        text: "a passage's view can move differently from the song's words; a "
                            + "Cut here is a decision, not an absence, and it stops the walk. "
                            + "A background that is a VIDEO still cuts whatever these say."
                    }
                }
            }
        }
    }

    // The family picker (task TS.4). It lists ONLY the audience worker's own
    // families — the ones its shaper can actually resolve — because a picker
    // of the operator's desktop fonts would offer names the show silently
    // falls back from. Search narrows the (still-arriving) list live; the
    // sample renders the hovered family in its own face; choosing writes the
    // family name through the ordinary typed-field verb, so it is exactly as
    // if the operator had typed it. It opens only for a worker that named
    // font.list; a disconnected worker leaves the typed field, which needs
    // nothing of the sort.
    Popup {
        id: fontPopup
        objectName: "themeEditorFontPopup"
        x: Math.max(0, (themeEditor.width - width) / 2)
        y: 60
        width: 360
        height: 440
        modal: true
        focus: true
        padding: 12
        background: Rectangle { color: "#2b2b2b"; border.color: "#555555"; radius: 4 }
        // The page standing down (mode exit, another page) takes its popup
        // with it — an orphaned modal overlay would keep the keyboard the
        // shell is trying to hand back.
        Connections {
            target: themeEditor
            function onVisibleChanged() {
                if (!themeEditor.visible)
                    fontPopup.close()
            }
        }
        Column {
            width: parent.width
            spacing: 8
            Text { text: "Fonts the audience's worker can draw"; color: "#dddddd"
                   font.pixelSize: 12; font.bold: true }
            PanelField {
                id: fontSearchField
                objectName: "themeEditorFontSearch"
                width: parent.width
                placeholderText: "search families…"
            }
            Row {
                spacing: 8
                Rectangle {
                    width: 90; height: 40; color: "#ffffff"; radius: 3
                    Text {
                        anchors.centerIn: parent
                        font.family: fontPreviewList.currentFamily
                        font.pixelSize: 20
                        color: "#000000"
                        text: "Ag"
                    }
                }
                Text {
                    objectName: "themeEditorFontListStatus"
                    width: parent.width - 98
                    anchors.verticalCenter: parent.verticalCenter
                    wrapMode: Text.WordWrap
                    color: "#888888"
                    font.pixelSize: 10
                    text: worker.fontListState === "loading" ? "asking the audience's worker…"
                                                              : ""
                }
            }
            ListView {
                id: fontPreviewList
                objectName: "themeEditorFontList"
                width: parent.width
                height: 300
                clip: true
                property string currentFamily: ""
                // Re-derived whenever the query changes OR another page of
                // the walk lands — the two triggers the list can move on.
                model: themeEditor.fontMatches(fontSearchField.text)
                Connections {
                    target: worker
                    function onFontListChanged() {
                        fontPreviewList.model = themeEditor.fontMatches(fontSearchField.text)
                    }
                }
                delegate: ItemDelegate {
                    width: fontPreviewList.width
                    objectName: "themeEditorFontRow"
                    required property string modelData
                    text: modelData
                    font.family: modelData
                    onClicked: {
                        themeEditor.applyField("fontFamily", modelData)
                        fontPopup.close()
                    }
                    hoverEnabled: true
                    onHoveredChanged: if (hovered)
                                          fontPreviewList.currentFamily = modelData
                }
            }
            PanelButton {
                objectName: "themeEditorFontCancel"
                text: "Close"
                onClicked: fontPopup.close()
            }
        }
    }
}
