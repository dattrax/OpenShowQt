# OpenShowQt progress and handoff

Updated: 2026-11-05 (Europe/London).

## Current checkpoint

**Latest remediation: CR.12 complete; CR.1–CR.8 and CR.10–CR.11 also delivered.**
The populated capture tour exposed a real shutdown use-after-free: RpcClient's
socket destructor emitted disconnected after its timers and supervisor font data
were gone. The explicit destructor stops timers/disconnects its socket callbacks
while members are alive. A connected-client destruction regression and controller
suite pass; the populated worker capture now exits cleanly. The earlier fresh
full gates passed 67/67 desktop and 36/36 enabled headless tests, before this
additional fix. **Next: CR.13**, a parser token lifetime issue found by the same
memory trace, then finish CR.9 capture/full-gate verification.
Fix and push each item separately. Verification uses
`/tmp/openshow-cs-remediation`, excluding the existing TV.3 changes. Only the
new sample-colour metadata hunk in DocumentController is part of CR.4; its
pre-existing TV.3 context changes remain unstaged.

**Review update, 2026-11-05:** the [CS conformance assessment](docs/reviews/CS-conformance-2026-11-05.md)
found preview-card, Bible-list/form, inspector and thumbnail scheduling defects
despite the focused tests passing. The earlier CS.9 visual sign-off below is
disputed by those findings; retain its historical evidence, but resolve the
review and the operator's forthcoming visual bug list before relying on that
sign-off. Assessment only: application code and existing TV.3 edits untouched.

**CS.9 complete (2026-11-05): one integrated tour walks the shared control set
across hosts — cross-host writes, per-slide customization, keyboard discipline,
hidden-page silence, browsing purity and a clean walk-back — and the five-page
capture matrix runs at every ledger size and scale (this commit).**

`ShowPage` gained one telemetry property, `bandSent` (an ask that actually
reached the worker, beside CS.8's `bandRequests`), and the suite grew one
CS.9 block that walks the seams CS.1–CS.8 only pinned per-part: a Songs size
written ON the Themes host through the visible field lands in the Edit host's
resolved field; the slide then customizes to 96 while the THEME row keeps its
48 — the chip wakes and announces "Revert fontSize customization" (read via
`QQmlExpression`, since attached properties need the QML engine, not
`QQmlProperty`) and Remove-customizations lands back on the theme's 48. Tab
walks on from the size field instead of squatting; Escape leaves typed text
in place and commits nothing; leaving Edit takes the cursor with the page and
commits the pending type as exactly ONE ordinary edit. At 1000×680 the rail's
first and last inspector controls both reach the viewport by scrolling the
rail. Across the whole tour the HIDDEN Show page's `bandSent` never moved
(the paired live catch-up check runs with the real transport further down
the suite), browsing — theme rows, view tiles, page switches, a cue click —
moved no document, no history and no output byte, and the undo walk-back
returns the record to the exact shapes it started with.

Three as-built truths the tour fought out, worth knowing before touching
this area again: a document write RE-VIVIFIES the rail, so every later use
of a cached `QQuickItem*` must re-resolve or it clicks where its corpse last
lay; the theme nav rows only exist on the Themes host, so re-selection is a
Themes-page act; and at 1380×800 with the drawer open, a mouse press aimed
at a stale rail coordinate lands on the media bin's 40px toggle bar — the
tour therefore drives the shared fields the way keys do (scroll, focus,
type, commit) instead of pressing near the rail's edge. One cosmetic
observation recorded, not fixed: at 1000 width the TopBar's "Unsaved
changes" text crowds the More tab on the Themes page (pre-existing, TopBar,
flagged for the TV leaves).

Gates: desktop **66/66**, headless serial **47/47**, `git diff --check`
clean, zero QML warnings in every run. Capture matrix produced for all five
pages at 1680/1380/1000 and re-run with `QT_SCALE_FACTOR=1.5` and `2`
(distinct prefixes; Show is the base/`.1380`/`.1000` set, Themes both views
the `.themes-*`/`.themes-bible-*` set, Bible Import and Edit the new
`.bible-*`/`.edit-*` set). Visual inspection of the set is overlap-free;
the one crowding observed is the TopBar note above. Save/reopen of the
records these controls write stays pinned by `document_controller_tests`
round-trips.

**Next action:** TV.3 (Bible view style fields over the CS.4 adapter), then
TV.4a → TV.4b → TV.5 → TA.1 — the leaves CS.9 records as the open FO gate.

### Earlier checkpoint: CS.7 + CS.8

**CS.7 + CS.8 complete (2026-11-05): the Show grid is the shared grouped
collection, and the thumbnail ask and scroll-to-cue both live on its mapped
cell geometry — bottom clamps and zoom steps included (CS.7 `ac1c34a`, CS.8
this commit).**

`ShowPage` stands one `SlidePreviewCollection` (`grouped: true`,
`stretchCells: true`, `cardWidth: thumbCellWidth`) where the hand-rolled
row-span Repeater was: groups cut on `entryId` (two copies of one show are
TWO groups), every occurrence opens at column zero, leftover row columns
become inert filler cells — no name, no picture, no clicks (pinned: a click
on padding selects and presents nothing). The collection is a GridView over
a computed visual-CELL model (`layout = {cells, cellOfCue}`, one generation
per layout change) with `cueRow`/`cueRect`/`scrollCueIntoView` and
`visibleIndexes` on top of it; the delegate context is the wrapper
`{cueIndex, row}` (`operatorCueTile` re-exports a cue-equal `index`), and
the host input renamed `model`→`source` (Bible import updated in the same
change; the rename is documented in the catalogue's migration map). Keyboard,
live/pending identity and the flat cue order are unchanged across the row
breaks — Next walks Alpha into Beta in one step, clicking a wrapped cue
picks that cue, an empty show adds no phantom cell, and browsing one show
from the library stays ONE uninterrupted group.

CS.8 closed the ask and the scroll on that geometry. The requested band is
the view's mapped window — viewport rows plus ONE bounded look-ahead row,
filler gaps never — with the 32-scene cap (bottom-anchored at the order's
end), per-slide dedupe and burst coalescing on top: five grouped scroll
jumps inside the timer window ask for nothing, the burst lands exactly one
ask, and it describes the destination. The bottom case ends the band on the
order's last cue and the cue step's scroll ends flush at the bottom; the
cap's cutting edge itself stays pinned in UI.3c over grouped rows (the
festival-twice 32-hit assertion now reads the cell window). Every thumb
size re-derives band, requested pixel size (`bandPixels` follows the live
`cellWidth`) and contained cells: a group head and a deep-in-last-group cue
scroll contained at all five zoom steps. One as-built truth worth knowing:
regenerating the cell model restarts the GridView layout at the top, so
after a zoom the ask follows wherever the view sits — the suite pins the
re-derivation, then scrolls back down to pin the anchored cap.

Gates: desktop **66/66** (the CS.7 block covers group identity, padding
inertness, wrapped clicks, browsed-ungrouped; the CS.8 block covers the
window contract mirrored from public properties, burst coalescing, the
bottom and zoom cases; `workspace_ui_tests` TIMEOUT 30→60 — the run
measures ~31s now), headless **36/36**, `git diff --check` clean, zero QML
warnings (the suite fails on any).

**Next action:** CS.9 — integrated reuse/accessibility/capture/regression
acceptance, capture matrix at 1000×680 / 1380×800 / 1680×1020 incl. scale
1.5/2; the still-TODO TV.3/TV.4a/TV.4b/TV.5/TA.1 scope question gets
flagged to the user at CS.9, not answered silently.

### Earlier checkpoint: CS.6

**CS.6 complete (2026-11-04): the Edit page's appearance rail is the shared
inspector — the slide now reaches the same typography and effects as the
Songs rail, every write customizes only this slide, and every revert path
lands back on inheritance.**

`EditPage` stands one `InspectorAdapter` (`target {kind:"slide"}`) at the
page root and one `AppearanceInspector` (`showGeometry:false` — the page
keeps its own `geometryField`/drag story and its `revertPlacementButton`
undo trail; `showTransition:false` — a slide has no per-slide movement;
`showRevertChips:true` — the Edit rail's badge tradition) between
`themeWornName` and `removeCustomizationsButton`. `pinEditNames()` assigns
the automation names the suite reads onto the parts that now own the rows:
`themeColourField`/`themeBackField`/`themeBackGradientField`,
`themeFontField` + the `editFont*` picker set, `themeSizeField`,
`themeSpacingField`, `editWeight(+Inherit)`, `editSlant(+Inherit)`,
`editFit(+Inherit)`, `editMinSize(+Inherit)`, `editStroke*`,
`editShadow*` — same names, new parts. One rename the tests record: the old
left/centre/right button trio is the shared alignment combo pinned
`themeAlignCombo`. Background image/fit stay page-owned slide props
(`backgroundCombo`/`backgroundFitCombo` → `setSlideBackground(Fit)`), not
inspector fields. The page's hand-rolled `commit()`/`valueOf()`/`fieldNames`
and ~200 lines of badge rows retired; `revertableNames` grew to the six
advanced fields plus placement, so Remove-customizations and its undo cover
advanced-only changes.

The chip contract (documented in the catalogue): `showRevertChips` +
`revertBadgePrefix` forward through `AppearanceInspector` to the colour
rows, the family row and the size/align/spacing rows; a chip's visibility
is ASSIGNED in each `sync()` from `state(field).canReset` and one click
resets that one field. Effects still revert whole through their Off
buttons, weight/slant/fit/min-size through their Inherit buttons.

The unchanged-focus-out rule lives in the TEXT rows (`commitIfChanged`,
`commitNumberIfChanged`, the picker's trimmed-equal rule, the sparse
own-equal skip), NOT in the adapter: a first attempt at an adapter-level
guard was reverted because a view seeding its own Crossfade off the
resolved value is a legitimate materialise (TR's pinned rule) and an
explicit combo pick that equals the resolved value is honest intent.
`InspectorAdapter::commit` carries a NOTE so the guard is not re-added.

Two write-path truths the tests pinned: the slide's TS.1 branch of
`customizeCurrentSlide` went through the strict map verb and refused what
QML actually sends — it now accepts an integral double as the same answer
an int would be, exactly the loosening `setThemeTextStyleField` documents
("700" and 400.5 stay refused); and Edit's weight combo needs the worker's
font list to have landed, same as the Themes rail. Layout lore: the
overlay ScrollBar covers the last ~16px of full-width rows, so the
inspector is `themePanel.width - 18` — chips at a row's trailing edge must
stay clear of it or clicks land on the bar.

Gates: desktop **66/66** (workspace_ui_tests grew CS.6 scenarios: chip
revert per field, weight typed → Inherit wakes → Inherit quiets, slant,
fit/min-size, stroke enable/Off, fork (`themeNewButton`) carrying the
slide's customizations into a new theme, capture-mode badge expectations
unchanged), headless **36/36**, `git diff --check` clean, zero QML
warnings (the suite fails on any).

**Next action:** CS.7 — group the Show previews by running-order entry.


### Earlier checkpoint: CS.5

**CS.5 complete (2026-11-04): one set of inspector parts, three rails, zero
duplicated arithmetic — and every automation name the suite ever read still
resolves.**

Seven shared parts now live flat in `src/ui/` (registered via
`OPENSHOW_WORKSPACE_QML_NAMES`, contracts rewritten in
`docs/ui/control-sets.md` to the as-built shape): `AppearanceInspector`
composes Background → TextStyle → BoxGeometry → TextEffects → Transition
for ONE `InspectorAdapter` (the target lives on the adapter, so browsing
moves `target` and every section re-reads itself through the part's
`Connections { target: adapter }` + explicit `sync()` — the adapter's
answers are function calls a binding cannot track); `TextStyleControls`
carries the `sparse` context spelling (own-values-only fields, `"(theme)"`
align entry and `""` fields that RESET rather than `commit(field, null)`,
no Browse/status/style-block rows a context cannot store);
`WorkerFontPicker` keeps the typed-name-always / worker-list-only rules
with the popup opened against the HOST page; `TextEffectsControls` keeps
the TS.4 materialise/merge/Off and release-only slider semantics;
`BoxGeometryControls` commits four fields as one `"box"` unit with shape
validation converged to finite numbers and w/h ≥ 1 (replacing the context
rail's older `w > 0` — documented in the catalogue); `TransitionControls`
serves the Songs rail, the Bible rail and the Settings service section
with one set of rules (`allowInherit`, `showKindHint`, `silentSuffix`,
host-pinned `note` text).

`ThemeEditorPage`'s two rails are now `AppearanceInspector` over
`themeAdapter`/`contextAdapter` (targets bound to `selectedId`); every
former `themeEditor*` rail objectName is re-pinned in
`Component.onCompleted`→`pinRailNames()` from the part that now owns the
row, so the suite (and muscle memory) reads the same identifiers. The
page's private arithmetic is gone: `syncStyleControls`, `applyStyleField`,
`commitSlider`, `styleValue`, `styleInherited`, `strokeShown`,
`shadowShown`, `applyPlacementFields`, `commitContext`, `applyContextBox`,
the whole TR.3 helper set, the page-level font popup, and the Settings
page's `transitionRecord`/`syncTransition`/`msOr`/`commitTransitionFields`
all deleted — ~500 lines of per-rail code replaced by three instances of
the shared parts. The Bible drags and "Theme's lyric box" commit `"box"`
through the adapter (its context merge retires a stored inset in the same
edit), making CS.4's centralized merge the ONLY spelling.

QML lore learned the hard way: inside an instance body, `worker: worker`
against a `worker` context property is a SELF-binding that reads
undefined — the page exposes `readonly property var workerApi: worker` at
its root and the rails bind to that.

Gates: desktop **66/66** (workspace_ui_tests unchanged — same pinned names,
same pinned behaviours, now served by the parts), headless **36/36**,
`git diff --check` clean.

**Next action:** CS.6 — migrate the Edit page's slide rail onto
`AppearanceInspector` over a `{kind:"slide"}` adapter; customization verbs
and the revert paths already exist in the adapter.

### Earlier checkpoint: CS.4

`InspectorAdapter` (`src/ui/InspectorAdapter.h/.cpp`, a `QML_ELEMENT` in the
`OpenShow` module) maps the controller's existing reads (`themes()` rows,
`themeSampleAppearance`, `themeContext`, `slideAppearance`,
`serviceDefaultTransition`) into the catalogue's per-field shape
`{value, ownValue, isSet, source, editable, canReset}` for four explicit
targets — `{kind: "theme"|"context"|"slide"|"service", id, view}` — and
routes `commit(field, value)`/`reset(field)` onto the hosts' existing verbs,
which stay separate, atomic and undoable. `source` is a token
(`"this theme" | "this view" | "this slide" | "the service" |
"the renderer default" | ""`); unset style/effect values report the
contract defaults (the domain's own constants, so "unset" and "draws today"
cannot drift); refusals return false AND emit `refused(field, reason)` —
the TV.3 colour wall on context rails, the slide's absent movement, the
concrete service floor, "an effect inherits only whole", "the theme is the
root of its own typography". `fields()` answers what a target can commit,
so CS.5 builds sections from capability.

The context's WHOLE-entry merge (read back the entry's own set, replace one
member, write once through `applyThemeContext`; a box write retires the
legacy inset in the same edit — and vice versa) is centralized here;
ThemeEditorPage's private `commitContext` arithmetic keeps its deliberately
identical copy until CS.5 moves the page onto the adapter, making that
migration a move rather than a change. The slide's box routes through
`setTextBoxGeometry` — the same single `placement` customization a drag
records — while `customizeCurrentSlide`/`revertCurrentSlideField` carry
every scalar and whole record; effect MEMBERS merge once into the record
the row already carries.

Registration lore learned the hard way: a dynamic `qmlRegisterType` into
the URI of a STATIC qml module shadows that module's own types (`PreviewView
is not a type`) — new C++ QML types belong in the module's `SOURCES` with
`QML_ELEMENT`, like `PreviewView`, in the app and the interaction fixture
alike; the headless adapter test compiles the same `.cpp` directly.

Gates: desktop **66/66** (new `inspector_adapter_tests`: four fixtures —
the service floor, the theme's every route, the context's merge/preservation
and refusals, the slide's customization/revert/box/member rules — over the
REAL controller), headless **36/36**, `git diff --check` clean.
workspace_ui_tests gained a pin that a QML FILE resolves the type and gets
the canonical shape back through the same namespace the app loads.

**Next action:** CS.5 — extract `AppearanceInspector` and the
style/effects/background/box/transition/font sections from the Theme
Editor's rails and Settings' transition section onto the adapter.

### Earlier checkpoint: CS.3

**CS.3 complete (2026-11-04): the Bible page shows the worker's pictures it
had always promised — keyed to a plan that can die honestly.**

`BibleImportPage` is restructured to the CS.1 shape: notice and status first,
a bounded scrolling verse list (`bibleVerseList` — checkboxes stay source
selection controls with their readable text), the generated plan as a
`SlidePreviewCollection` in its **ungrouped mode** (new file; `grouped` stays
false until CS.7's row arithmetic) whose delegates are `SlidePreviewCard`s on
the keyed **Thumbnail** target, and a substantial **off-air canvas**
(`biblePreviewCanvas`) on the **Selection** target. The old sentence
promising pictures "in the preview panel on the right" was never true of the
Audience monitor; the footer now says what actually happens and where.

`ScriptureImport` grew the plan's transient identity: `planGeneration` bumps
on every publish/Add/cancel/failure, `previewKey(i)` is
`scripture:<plan>:<index>` (a new plan cannot inherit an old plan's cached
frames; the key is session vocabulary, never persisted), `previewSceneJson(i)`
answers from the one compile done at publish (same
`scriptureSlideSceneJson` the preview and Add use), and `selectedIndex` says
which card the canvas paints. Add and Cancel now stand the selection down
with the plan — no card keeps a "current" label for words the schedule
absorbed. The page owns both lanes while visible and clears both on exit, so
Show/Themes/Bible batches cannot race each other across a page turn.

Correlation landed at the seam the plan asked to audit:
`PreviewConsumer::selectionCurrent` (mirrored by WorkerController, and the
consumer now announces ask-time with `changed()`), distinguishing "a frame is
in hand" from "the held frame answers the newest ask". The Bible canvas masks
on it between plans; the Edit/Themes canvases keep their steadiness and
existing readiness untouched.

Gates: desktop `ctest -j1` **65/65**, headless **36/36**, `git diff --check`
clean. New pins (bible_import_page_tests): keys name generation and slide and
never collide; republish retires the old keys; Add retires cards, keys and
the off-air picture together; selection masks at ask time and the mask lifts
when the selected slide's own frame answers (run where the split guarantees a
second slide, so it cannot be skipped). workspace_ui_tests: the page carries
the verse scroll, the card collection and a Selection-only canvas; existing
BIB lookup/pagination/Add/persistence journeys untouched.

**Next action:** CS.4 — the `InspectorAdapter` read model
(`{value, ownValue, isSet, source, editable, canReset}`) mapped once from the
existing controller reads, with `commit`/`reset` routed per target.

### Earlier checkpoint: CS.2

**CS.2 complete (2026-11-04): four hand-built preview panes became one surface
and one card — and every pane still paints only its own worker target.**

`SlidePreviewSurface.qml` wraps `PreviewView` (still the only pixel painter)
with the arithmetic four pages used to keep re-inventing: the fitted
`contentRect`, `zoom` for authored-unit overlays, an honest `placeholder` that
names WHY there are no pixels, `pixelsVisible` for scenes a page does not hold,
and an overlay slot pinned to the fitted rect so box outlines and placement
handles ride the picture through a resize. `SlidePreviewCard.qml` puts that
surface in selectable chrome — hover body, worker-LIVE vs operator-selected
border, caption strip, `activated`/`contextRequested` — while what a click
MEANS stays with the host. Migrated in this leaf: OutputPanel's Audience
monitor (`audiencePreview`, `previewOfflineNote` preserved), EditCanvas's
selection stage, the Theme Editor's centre canvas and both view tiles, and the
ShowPage cue tiles (now `SlidePreviewCard` delegates keeping `operatorCueTile`,
`index`, `modelData`, the ack chip and keyboard semantics). Registered flat
through `OPENSHOW_WORKSPACE_QML_NAMES` per the CS.1 convention.

Readiness needed one C++ fact the surfaces could trust: `PreviewView::hasFrame`
(NOTIFY, refreshed on the paint path) — a property of the image THIS item
painted, including a keyed thumbnail's own arrival, so no tile can report
another target's frame as its `ready`. The catalogue records two contract
refinements alongside: `contextRequested` carries card coordinates (the host
maps), and a card's text fallback is the surface's placeholder, not a second
input.

Gates: desktop `ctest -j1` **65/65** (~409 s), headless build + `ctest -j1`
**36/36**, `git diff --check` clean. New workspace_ui_tests pins: the cue card
captions its own row and names the thumbnail target and its own key; the
painter under each pane reads Audience/Thumbnail exactly as bound and nothing
else; unready surfaces hold their placeholders with no worker on the wire;
the view tiles are keyed `theme-view-songs-<id>` thumbnails; the monitor
claims no frame it lacks.

**Next action:** CS.3 — the Bible import workspace: generated-slide cards and
a labelled off-air Selection canvas on the compiled scripture appearance, keyed
`scripture:<planGen>:<index>` thumbnails, corrected preview wording.

### Earlier checkpoint: CS.1

`docs/ui/control-sets.md` is the contract document PLAN.md CS.1 promised: for
every named part (surface, card, collection, five control sections, picker, and
the three host-owned areas) it records the exact pixel target(s) it may display,
where its values come from, which controller verbs its signals must reach, which
hosts consume it, its accessibility name and stable test names, and what remains
owned by the page. It also freezes the rules the extraction leaves depend on:
`PreviewView` stays the only pixel painter; new QML files are flat in `src/ui/`
registered through `OPENSHOW_WORKSPACE_QML_NAMES` (the test modules flatten
filenames, so subdirectories would break both); every instance carries a stable
unique `objectName` and `Accessible.name`; no persisted component identifiers;
parts never schedule requests, never present, and never invent inherited values.
The per-field inspector read model `{value, ownValue, isSet, source, editable,
canReset}` and the `commit(field, value)`/`reset(field)` event pair are specified
for CS.4, mapped to the existing verbs per host so theme/context/slide writes
stay distinct, atomic and undoable.

Catalogue only — no behaviour change, so no build gate beyond the existing green
tree was needed.

**Next action:** CS.2 — extract `SlidePreviewSurface.qml` + `SlidePreviewCard.qml`
and migrate OutputPanel's Audience monitor, EditCanvas's stage, ThemeEditorPage's
`previewFrame`/`ViewTile` and ShowPage's tile delegate, keeping targets bound
explicitly and overlay coordinates inside the fitted rectangle. Then CS.3.

### Earlier checkpoint: TR.3

**TR.3 complete (2026-11-04): the operator types a number and the audience feels
it — measured in frames, not in a widget's `text`.**

The transition contract was two leaves of plumbing: TR.1 got the record to the
socket intact and TR.2 got the renderer to obey it. Both were honest that nobody
could *set* one. This leaf opens the two doors and then refuses to call that
proof until a real worker's preview frame was timed, because the acceptance says
out loud not to infer success from a widget value.

**One author for the shape, so a read cannot offer what a write refuses.**
`DocumentViews::transitionRecordMap(optional<TransitionOptions>)` is the single
place a record becomes QML — `{kind, textDurationMs, mediaDurationMs}`, or an
EMPTY map for a view that says nothing, because there is no half-record default
to invent. `insertThemeTransitionRow()` puts `transitionSet` beside `transition`
on every `themes()` row AND on `currentTheme()`, and `themeContext()` carries the
pair for the named context. `DocumentController`'s private `transitionToMap` is
deleted rather than kept beside it: two spellings of one record is how a page
ends up showing a key its own verb would turn down.

**Movement travels with the entry it belongs to.** `applyThemeContext` REPLACES a
`TextContext`, so it now carries the existing entry's `transition` across when
`fields` names no movement. Absent keys are silence about the field they name —
and a caller that came to change the reading font named nothing about how the
view arrives. Without that carry, editing a passage's font would have taken that
view's Crossfade off in silence, which is the kind of loss a document spends
years explaining. `transition` is therefore the ONE field whose absent keys do
not silence it, and `DocumentController.h` says so where the next hand will read
it.

**Three choices and no fourth, in both doors.** Settings holds the service's
standing answer; the Theme Editor's Songs section writes the Theme's record and
its Bible section the `scripture` context's. Inherit *writes nothing* — the
removal — and is enabled only against a record that exists, because a button
writing "nothing" over nothing costs an undo for no change. Cut and Crossfade
each write a WHOLE record seeded from what was on show: the file, the wire and
the domain all refuse a fragment, so one gesture is one undo and a half commit is
not something a page can do by accident. A Cut **disables** the two durations
without erasing them, so coming back to Crossfade comes back to the times that
were there. A silent view shows what silence resolves to — the page reads the two
records in front of it (`context → theme → service`, the order a Go walks) rather
than re-walking a chain, so it cannot disagree with `transitionForSlide` — and
the source line names the seat that answers ("inherits this theme — the theme
carrying this view answers before the service") and says a Cut on its own terms
instead of quoting times it ignores. Validation splits where the knowledge lives:
QML checks SHAPE only (digits, no sign, no point — anything else is `-1`, never a
silently-rounded `0`), and the 0..10000 band stays `Documents::validTransitionOptions`'
one sentence, quoted by the settings refusal and by the worker's parse;
`document_present_tests` proves the same `10001` earns the same words over a real
socket. A refused or non-numeric commit re-reads the model, and an unchanged
commit writes nothing.

**The pixels were the second half of the acceptance, and they taught two things.**
`frontend_journey_tests` leg 10 drives `DocumentController` verbs through
`main.cpp`'s own resolver wiring to a real worker and times the operator's
preview: a service default of 2600 ms is seen as an early blend AND a landing no
earlier than 1700 ms; the view's Cut shows no frame holding both grounds and
lands in a fraction of the same slide's crossfade; changing the view mid-fade does
**not** shorten the fade already crossing while the very next cue is a Cut; an
explicit `clearAll` reaches black without crossing whatever the view's numbers
say. And the restore is where the fixture had to learn the contract instead of
restating it — `PresentationModel` resolves the record its retained content is
owed at the moment the CLEAR takes its snapshot, so clearing a crossing cue and
*then* cutting the view still restores the words over 2.6 s. The claim in the
test is now the truth in the code: a retained restore point owns its own record,
and an edit made afterwards changes nothing.

**Two clocks, one lesson.** `scenePending` sleeps when the present's *reply*
lands; `sceneActive` moves when the next status poll reports it, every 500 ms. The
"editing a theme nobody wears presents nothing" claim failed honestly for exactly
that reason: a baseline taken at the first quiet moment is the cue BEFORE the
operator's last one, so the fixture now waits for the acknowledged revision to
hold still for longer than a poll before freezing it. A second lesson from the
same leg: a repeat present of a revision already on air is a no-op by design, so
every timing claim mints its own revision — a no-op that reports the picture it
was already showing is evidence of nothing.

**The last clause, taken literally.** "Restart begins black" is measured, not
asserted: the leg shuts its worker down and brings a fresh one up in the same
frontend. Its own preview is uniformly black with no words and no ground on air,
its hello still claims `scene.transition-options.v1` (the contract belongs to the
build, not to a process that has seen tonight's cues), and the same slide still
answers 2.6 s — the milliseconds live in the document. (`production_output_tests`
keeps owning the audience's every-pixel black, footer band included; this is the
frontend's preview of the same fact.)

Evidence: `workspace_ui_tests` (Settings' kind/numbers/disable-without-erase/undo;
the Songs picker refusing Inherit until a record exists, Cut seeding from what was
shown, typing one duration keeping its neighbour, an unchanged re-commit costing
no undo, `10001` refused with the record unmoved, Inherit surrendering a record
and one undo bringing picker and record back; the Bible door showing the THEME's
90/800 with "inherits this theme" for a silent view, writing its own record,
naming itself as the source, keeping its Cut through a `contextSize` commit, and
handing movement back without touching the reading size — the fixture leaving
theme, context, view and window as it found them), `document_present_tests` (the
resolver's own answer for a UI-written theme goes on `scene.present` and LANDS; a
key-less present is still the 500/800 a pre-TR.1 frontend performs; a Cut carries
its durations whole; an over-band number is refused naming key and range with the
audience unmoved; an inherited theme resolves back to the service's pair and the
worker accepts it on the same keys), `document_controller_tests` (rows and
`currentTheme` carry the record, silence reads EMPTY, a typography-only write
preserves the movement, one undo per commit with a refusal costing none),
`frontend_journey_tests` leg 10 (a)–(h) above. Gates: desktop `ctest -j1`
**65/65** (~410 s), headless build + `ctest -j1` **36/36**, `git diff --check`
clean.

Earlier this plan: TR.2 (`b3f6dc4`) the renderer drawing the cue's own record and
the capability earned; TR.1 (`108b919`) the transition contract to the socket;
TS.5 (`c3f5e84`) the pinned offline font corpus; TS.4 (`32bcdb7`) worker-owned
`font.list` and one-field style controls; TS.3 (`c2e6645`) styled measurement
(`ink`/`fillInk`, `text.measure.v2`); TS.2 (`e4258e9`) the styled renderer; TS.1
(`c5acad7`) the style contract (service 8, scene 4, minor 15); TV.2 (`7331a10`)
the Themes layout; TV.1 (`0f1cf84`) sample-based previews.

## Where the work stands

Phases 0–5 complete. Phase 6 complete through 6.26i. The Bible plan (BA.1–BA.8)
is DONE. The themes plan: **TV.1, TV.2, TS.1–TS.5, TR.1, TR.2 and TR.3 DONE** —
the run this session was asked for ("Text styling and effects" and
"Configurable slide transitions") is closed, and the TR group with it. Remaining
in the plan but out of this run by request: TV.3, TV.4a, TV.4b, TV.5 and the
closing leaf TA.1 (architecture and captures sweep).

2026-11-04 planning update: PLAN.md now records the **CS shared Qt control-set
work before FO**, including themed Bible previews, common appearance controls
and a new preview row for each running-order entry. CS.0 (source review),
**CS.1 (the catalogue)**, **CS.2 (shared surface/card, all preview hosts
migrated)** and **CS.3 (Bible import on worker previews, plan-keyed, honest
off-air canvas)** are DONE; CS.4–CS.9 remain. **Next action: CS.4**, the
inspector read model and mutation adapters. Complete the remaining TV/TA and
M7 gates before FO.

## How to pick up

1. `PLAN.md` is the contract: ledger rows carry acceptance evidence, the themes
   runbook paragraphs give each leaf's boundary, and "Format allocation" records
   the current **9 / 4 / 0.18** (TR.2 and TR.3 claimed nothing — one changed
   pixels, the other opened doors onto records that were already stored).
   `docs/reviews/theme-editor-2026-11-03.md` is the design authority.
2. Build `cmake --build --preset desktop --parallel 2` (build dir `build/`);
   test `ctest --preset desktop -j1` (~7 min). Headless: `--preset headless`
   (`build-headless/`, 36 tests). Individual binaries: `./build/tests/<name>`;
   `text_measure_tests`, `worker_controller_tests` and `live_fade_tests` take the
   worker binary as argv[1] (running a UI binary straight from `build/tests`
   without ctest's environment fails at "no worker", which is the fixture, not a
   regression). ctest names are target-ish, not file-ish: `rpc_ipc_tests` runs as
   `worker_rpc_lifecycle`. Rebuild after renderer/protocol changes — a stale
   binary once faked a green suite.
3. **The transition surfaces, all of them, in one place**: the verbs
   `setServiceDefaultTransition`, `setThemeTransition` (empty map = inherit),
   `setTextContextTransition`; the reads `serviceDefaultTransition()`,
   `transitionForSlide(showId, slideId)`, `transitionForSceneJson(scene)` (what a
   Go asks, wired in `main.cpp`'s `setTransitionResolver`), and the row shape from
   `DocumentViews::transitionRecordMap`. The controls are SettingsPage.qml's
   TRANSITION section and ThemeEditorPage.qml's `themeEditorTransition*` (Songs)
   and `themeEditorContextTransition*` (Bible) controls, whose sync/commit
   functions are `syncTransition` / `commitThemeTransitionView` /
   `commitContextTransitionView`. Nothing there re-implements priority: the page
   reads records, the resolver walks the chain.
4. Transition lore this leaf earned, worth keeping:
   * `scenePending` (reply) and `sceneActive` (500 ms status poll) are **not** the
     same clock; sample an acknowledged baseline only after it holds still longer
     than a poll.
   * A repeat present of an already-active revision is a no-op, so a timing claim
     needs a fresh revision every time (`addSlide()` + `applyThemeToSlide` for a
     second payload).
   * White-pixel counting is unreliable across a fade OUT (it stops counting below
     α≈0.88) but fine across a fade IN; `uniformlyBlack` only trips near α≈0.96,
     which is what makes `msToBlack` a clean Cut-vs-2.6 s discriminator.
   * The restore's record is resolved at CLEAR time (`fRestoreTransition` beside
     `fRestoreJson`), not at Go time and not at restore time.
   * A second `WorkerController` in one process reuses the pid-named socket path
     safely: the exiting worker unlinks it. That is how leg 10 restarts a worker.
5. QML test lore, still true and now sharper: drive a `ComboBox` with
   `setProperty("currentIndex", n)` **and** `invokeMethod(item, "activated",
   Q_ARG(int, n))`; drive `CheckBox`/`PanelButton` with a real `clickAt`; a control
   at the bottom of the inspector rail must be scrolled to
   (`scrollInspectorTo` / `scrollInspectorToTop`) before it is clicked — clicking
   where it is *not* silently hits the wrong item, and forcing `enabled` on a
   control the model disabled tests a state the product never has. A closed
   `Popup` is not a `QQuickItem` (`window->findChild<QObject*>`). Flickable
   `contentY` is not clamped. In `workspace_ui_tests` there is no worker
   (`outputLive()` false, `sceneActive()` 0) and `docs.status()` is overwritten by
   an autosave refusal planted earlier — never assert a refusal through `status()`
   there; and an empty `transition` map means silence, never a filled-in default.
   QML changes only reach the binary through a FULL `cmake --build --preset desktop`.
6. Wire-test lore, still true: `scene.present`'s keys are `transition`,
   `textDurationMs`, `mediaDurationMs`, vetted by `parsePresentTransition` before
   the store is touched, and every refusal quotes
   `Documents::validTransitionOptions`. The scripted peer in
   `worker_controller_tests` logs every present verbatim as `presentOps` (a count
   cannot tell 100/200 from 900/1000) and capability-count tripwires live in BOTH
   `protocol_tests` and `rpc_ipc_tests` (21).
7. C fixture lore, still true: the font corpus is a prepared cache, not vendored
   (`tools/setup-font-fixtures.sh prepare --download`, then
   `ctest --preset desktop -R font_fixture_tests`; without it the test reports
   `Skipped`, which is the designed outcome). `curl` needs `--globoff` for those
   filenames; Boost 1.90 spells doubles in exponent form and JSON fixtures use
   decimal integers; `-fno-rtti` is required on any TU that includes Skia
   headers; scene JSON colours are packed integers, not `#rrggbb` strings; the
   snapshot CLI is `build/src/worker/openshow-worker-dev --snapshot out.png
   --scene-file s.json` at 1920×1080 for the 960×540 authored scene.
8. Never hold a `const Theme*` across a call that can grow the theme vector. Qt6
   has no `QMetaType::QMap`. A ctest "Passed" after a failed build means a stale
   binary — rebuild first, and grep piped builds for `error:|Error [0-9]`;
   multi-main UI tests print nothing on pass. Leg budgets worth knowing:
   `frontend_journey_tests` ~46 s of its 180 s TIMEOUT, `workspace_ui_tests` ~23 s,
   `document_present_tests` ~2 s of 90 s.

## Recent commits

- `3150e55` docs(ui): CS.1 - the control-set contracts and component catalogue
- `c627ef0` feat(ui): CS.2 - one preview surface, one slide card, four honest
  wearers
- `182996a` feat(ui): CS.3 - the Bible import on worker previews - plan-keyed
  cards, an off-air Selection canvas and an honest retirement of both lanes
- CS.4 (this commit) feat(ui): the InspectorAdapter - one field-shaped read
  model and one routed verb pair over the four inspector targets

- TR.3 — the transition UI with the audience as witness:
  `DocumentViews::transitionRecordMap` as the one author of the record's QML
  shape (`transitionSet` + `transition` on `themes()` rows, `currentTheme`,
  `themeContext`), `applyThemeContext` carrying the entry's movement across a
  replacing write, Settings' TRANSITION section, the Theme Editor's Transition
  section for Songs and for the Bible context (Inherit writes nothing and is only
  offered against a record that exists, Cut/Crossfade write whole records,
  durations disabled without being erased, the inherited source named, the
  video-background limit stated beside the background duration), QML validating
  shape only while the domain owns the 0..10000 band, and evidence in
  `workspace_ui_tests` (real gestures), `document_present_tests` (the record the
  controls wrote lands on a real socket) and `frontend_journey_tests` leg 10
  (timed pixels: default inheritance, view override, active-fade stability,
  off-air edits cost nothing, clear is a Cut, restore keeps the snapshot's record,
  ack-backed live markers, persistence, and a restarted worker streaming black).
- TR.2 (`b3f6dc4`) — the renderer draws the cue's own record: `setActiveScene`
  takes `TransitionOptions` with no default argument, `SceneTransition` carries
  both tracks' seconds converted once at land time, `fadeBlend` checks `duration
  <= 0` before the elapsed rule, the library's fade constants and the fade/cut
  enum and narrowing seam are retired, explicit clears present a Cut and a
  restore moves with the record captured beside its retained content, video keeps
  taking the media cut (documented and tested in both directions), and
  `scene.transition-options.v1` joins the hello at 21 capabilities.
- TR.1 (`108b919`) — the transition contract landed to the socket:
  `domain::TransitionOptions` with one shared validator, service default +
  Theme/TextContext overrides, `resolveTransitionForSlide` (context → first theme
  in chain order → service), service format **9** with the root record required
  and pre-9-era refusals at all three sites, options captured at Go and carried
  as a value through the prepare intent to `scene.present`, protocol minor 18
  with the capability defined-but-withheld, and the duration gate that downgrades
  and speaks rather than stranding the audience.
- TS.5 (`c3f5e84`) — pinned offline font corpus (manifest + setup script + hash
  checks), `font_fixture_tests` over an isolated fontconfig configuration (family
  resolution before pixels, four-face distinctness, weight ramp, body/caption
  metrics, extreme legal effects against the ink bounds, fixed vs shrink,
  missing-family fallback inside the corpus, coverage matrix, midpoint fade per
  pass, cached scan / cold-and-steady shaping / bounded layout-cache records),
  visible skip vs corrupt-corpus failure, and `docs/fixtures/fonts.md`.
- TS.4 (`32bcdb7`) — worker-owned `font.list` (minor 17, `font.list.v1`,
  capped paging, request-id walk retirement), the one-field
  `setThemeTextStyleField` merge verb, inspector typography/effect controls and
  font picker with commit-on-release gestures, and the fixed inspector-rail
  scroll helpers.
- TS.3 (`c2e6645`) — styled measurement end to end: `ink`/`fillInk` split,
  `fits`+`bounds` replies, `fitMode:"fixed"`, minor 16 + `text.measure.v2`
  gate, shared full-style cache keys, flow verdict criterion.
- TS.2 (`e4258e9`) — faces/stroke/shadow through public Skia, fade-aware
  painter, measured device-space blur rescale, `scene.text-style.v1`.
- TS.1 (`c5acad7`) — shared text-style contract, service 8, scene 4, protocol
  minor 15, capability gate, controller surfaces.
- TV.2 (`7331a10`) — full-workspace Themes layout, view thumbnails.
- TV.1 (`0f1cf84`) — theme sample views through the preview transport.
