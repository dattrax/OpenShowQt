# Shared Qt control sets — component catalogue

Created by CS.1 (2026-11-04) as the contract record for the CS milestone
(PLAN.md “Shared Qt control sets and preview corrections”). This is a
component catalogue, not a task ledger: it records each part's pixel targets,
inputs, events, adapter, consumers and automation names so the extraction
leaves (CS.2–CS.8) cannot diverge on what a named part means. The review and
boundaries live in PLAN.md; the leaf statuses live in the PLAN.md ledger.

Every part named below is implemented as **QML composition with explicit
properties and signals** — never a page-inheritance hierarchy. Theme/view/
slide appearance inheritance belongs to the Qt-free domain resolver
(`appearanceForSlide`); QML must not reimplement that chain, only render and
write back what the controller's rows and verbs expose.

## Ground rules (all parts)

- **Display-only surfaces.** A reusable preview part shows worker pixels and
  reports geometry; it never schedules scene requests and never presents.
  The page/session owner decides what activation does. A Show card may
  present; a Bible or Themes card only selects off air. No reusable preview
  part carries a default Go/clear operation.
- **`PreviewView` stays the only pixel painter.** The worker's shared-memory
  streams, the seqlock copies and the thumbnail cache are unchanged by this
  catalogue; every surface wraps the existing `PreviewView` item.
- **Honest placeholders.** Missing thumbnail or selection pixels show a text
  placeholder naming the reason (no worker, waiting for the render, worker
  cannot draw off air). A surface never substitutes the Audience frame and
  never paints Qt text in place of the rendered slide.
- **Overlays ride the fitted content rectangle.** Any overlay (selection
  outlines, placement handles, badges) transforms by the frame's own
  `width/1920` of the *fitted content rectangle* — the letterboxed rect the
  pixels actually occupy — so handles track the picture under resize and
  letterboxing. Controls must not obscure the content or intercept card
  actions that are not theirs.
- **Browsing never writes.** Selection, view switching, scrolling and popup
  opening cost no undo step and no document change, and send nothing to the
  audience.
- **No persisted component identifiers.** `objectName`s and
  `Accessible.name`s are source/UI vocabulary for tests and assistive
  technology, never service-format keys.

## Pixel targets and the wire envelope

| Surface target | `PreviewView.Target` | Fed by | Rules |
|---|---|---|---|
| Audience | `Audience` | `WorkerController::previewImage()` (the presenting mirror) | Only the AudienceMonitor shows it; clears live beside it in `OutputPanel`, never inside the surface |
| Selection | `Selection` | `WorkerController::requestSelectionPreview(sceneJson)` / `clearSelectionPreview()` | Off-air render of one scene. Readiness is `worker.selectionReady`; CURRENTNESS is `worker.selectionCurrent` — whether the held frame answers the newest ask (CS.3): pages that must not caption a dead plan mask on it. One page owns the lane at a time (the visible page: Edit, Themes and Bible each stand down on exit and re-ask on entry) |
| Thumbnail | `Thumbnail` + `key` | `WorkerController::requestThumbnails([{key, scene}], w, h)` / `clearThumbnails()` | Bounded batch, REPLACES the previous ask; cap 32 scenes; keyed arrival via `thumbnailReady(key)`. Keys must be stable and unique per producer (see each part) |

Every thumbnail ask is 16:9 with each dimension inside **32..512** — the
largest accepted frame at the 640×360 slot this app streams at — computed
from the tile's own picture width so a size a control picks can never be
refused on the wire (`ShowPage::bandPixels`, `ThemeEditorPage::viewThumbPixels`
are today's spellings of that rule).

CR.7: Show and Bible submit the complete visible/look-ahead set through
`WorkerController::requestThumbnailWindow`. PreviewConsumer deduplicates it
before batching, skips cache-current scenes, and continues in packets of at
most 32 after each packet's accepted pictures arrive. Refused entries do not
block later packets. Viewport metadata is capped at 128 distinct scenes and
the frontend picture LRU at 128 entries / 8 MiB; wire limits are unchanged.
Replacing the window, calling the legacy single-batch API or standing down
cancels its queue. Show stands down when hidden. Settled windows send no loop
of repeat requests; a reconnect retires old pixels and replays current demand.

CR.8 originally tied Bible Selection ownership to page visibility. The later
BIU.1 simplification removes its duplicate canvas: BibleImportPage now keeps
`ScriptureImport.previewActive` false throughout its lifetime and owns only
thumbnail demand. Standalone session callers can still explicitly use Selection;
Edit and Themes continue to own that lane when visible.

## Registration and naming convention (CS.1)

- Every new QML file is **flat in `src/ui/`** and registered by adding its
  name to `OPENSHOW_WORKSPACE_QML_NAMES` in `src/ui/CMakeLists.txt`. The
  loop there sets `QT_RESOURCE_ALIAS` to the bare filename, and
  `tests/CMakeLists.txt` reuses `OPENSHOW_WORKSPACE_QML_FILES` unchanged for
  the `workspace_ui_tests` module — so a subdirectory layout would break the
  flattened aliases unless BOTH modules were deliberately updated. Flat is
  the convention; a new part that skips the list simply does not exist to QML.
- Each extracted part file starts with a header comment naming its contract,
  e.g.
  `// UI part: SlidePreviewSurface; contract: docs/ui/control-sets.md#slidepreviewsurface`.
- `objectName`s stay **stable and unique per instance**. Preserve existing
  automation names when migrating (`audiencePreview`, `operatorSlideGrid`,
  `themeEditorPreview`, …); a page may instantiate a shared part with an
  explicit `objectName`. Where two hosts need distinct names, use the host
  prefix that page's tests already read (`themeEditor*`, `bible*`,
  `operator*`, `edit*`). Any legitimate rename is documented in this file in
  the same commit that changes the tests.
- Interactive controls carry a meaningful `Accessible.name`; state changes
  (live/pending, missing font) extend the name rather than replacing it.

## Read model (CS.4) and write routing (CS.4–CS.6)

Shared style controls do not read theme chains. One adapter (the
`InspectorAdapter`, `src/ui/InspectorAdapter.h/.cpp`) maps the existing
controller reads (`themeSampleAppearance`, `themes()` rows, `themeContext()`,
`slideAppearance`, `serviceDefaultTransition()`) into one per-field shape:

```
fieldState(target, field) -> {
  value,      // what the audience draws today (resolved), for display
  ownValue,   // what this target stores itself, when isSet
  isSet,      // does THIS target carry the field
  source,     // who answers when silence falls: "this theme", "this view",
              // "this slide", "the service", "the renderer default", ""
  editable,   // can this target store this field at all (capability)
  canReset    // is there an own value to remove (the Inherit button)
}
```

Since CR.4, theme display values come from the compiled sample rather than
substituted contract defaults; `ownValue` remains the raw stored value. A silent
optional style/effect names `the Default theme` when that floor carries the
record, otherwise `the renderer default`. No appearance resolution lives in QML.

Implemented in CS.4 in this shape, with `ok` false plus a `reason`
when the TARGET itself is missing (unknown theme id, no such context, no
current slide) and a `reason` on non-storable fields (TV.3 colours, the
slide's absent transition, the concrete service floor). The adapter also
answers `fields()` — the spellings THIS target can commit, so a section is
built from capability — relays `changed()` off the controller's
documents/navigation edges in whichever order host pages bind `document`
and `target`, and emits `refused(field, reason)` for every declined
commit/reset: a refusal changes nothing (the verbs guarantee that) and the
control shows the same sentence the status line keeps.

Field spellings are the domain's; the inspector adds `box`, which routes to
whatever a host calls its rectangle (`placement` on themes and slides, the
merged `box` on contexts — a context `box` commit whose map carries an
`inset` member switches the legacy spelling instead).

`target` is explicit identity, not “whatever is selected”:
`{kind: "theme"|"context"|"slide"|"service", id, view}` — theme identity,
view, slide and selected item stay distinct. Controls emit
`commit(field, value)` and `reset(field)`; the **adapter** translates
them to the domain verbs, which remain separate, atomic and undoable:

| Target | Scalars (family/size/align/spacing/weight/slant/fit/min) | Effects | Box | Transition |
|---|---|---|---|---|
| `theme` (Songs) | `editTheme` for the base fields, `setThemeTextStyleField` for the style block | `setThemeTextStyleField("stroke"/"shadow", null)` whole-record clear | `setThemePlacement` / `clearThemePlacement` | `setThemeTransition` |
| `context` (Bible view) | whole-entry merge through `applyThemeContext` (carries untouched members incl. `transition`) | not storable until TV.3 — state the inherited source and disable | merged `box` (retires `inset` in the same edit) | `setTextContextTransition` |
| `slide` (Edit) | `customizeCurrentSlide` + `revertCurrentSlideField` | same verbs with the effect-record fields | `setTextBoxGeometry` — the same single `placement` customization a drag makes | none (no per-slide override) |
| `service` | — | — | — | `setServiceDefaultTransition` |

A controls file must not walk theme chains or synthesize inherited
overrides to populate a field. Refused edits cost no undo; sliders/geometry
gestures commit once on release and undo once; a refusal re-reads the
authoritative record. **Unchanged focus-outs** are not edits: the TEXT rows
guard equality themselves (`commitIfChanged` / `commitNumberIfChanged` /
the picker's equal-family rule) and re-read the record instead of writing
(CS.6). The adapter deliberately carries NO equality guard — a commit whose
value equals the RESOLVED one can be a materialise (a view seeding its own
Crossfade off what the show already draws is TR's pinned case), and an
explicit combo/checkbox/picker choice that happens to match the resolved
value is honest intent, so it commits and writes. QML cannot put an int in
a QVariant, so both single-field weight doors (`setThemeTextStyleField` and
`customizeCurrentSlide`) accept an integral double as the same answer a C++
int would be; `"700"` and 400.5 stay refused on both sides.

## Parts

### SlidePreviewSurface

`src/ui/SlidePreviewSurface.qml` (extracted in CS.2). Aspect-fit worker
pixels in a frame, with status/placeholder and an overlay slot. Wraps the
existing `PreviewView`; it is the only place the fit arithmetic lives.
Migrated in CS.2: the Audience monitor in `OutputPanel`, the `EditCanvas`
stage, the Theme Editor centre canvas and view tiles, and the Show cue cards
through `SlidePreviewCard`. Readiness grew as `PreviewView::hasFrame` for it:
a property of the PAINTED image, so a tile with no pixels of its own never
reports another target's frame as its `ready`.

| | |
|---|---|
| Inputs | `controller` (WorkerController*, required); `target` (`"audience"` \| `"selection"` \| `"thumbnail"`, default `"audience"`); `key` (thumbnail identity, ignored otherwise); `aspectRatio` (default 16:9); `frameColor`, `borderColor`, `borderWidth` (hosts keep the frames their screens are known by); `placeholder` (string naming WHY there are no pixels) with `placeholderColor`/`placeholderPixelSize` and `placeholderObjectName` for the page's tests; `placeholderVisible` (defaults to `placeholder !== "" && !ready`; a host may override it to show a CAPABILITY statement — "this worker cannot draw thumbnails" — ahead of an in-flight ask); `pixelsVisible` (the page shows pixels only for a scene it actually holds); `clipOverlays` |
| Outputs | `contentRect` — the fitted, letterboxed rect **in this item's coordinates** (overlays anchor to it); `ready` — pixels for THIS item are in hand (`PreviewView::hasFrame`, refreshed on the paint path, so a keyed tile reports its OWN key's arrival, never another target's); `zoom` — `contentRect.width/1920`; `preview` — the wrapped `PreviewView` for hosts that map coordinates |
| Events | none of its own beyond the controller's (`ready` is a tracked property) |
| Adapter | none — display only. The owning page schedules `requestSelectionPreview` / `requestThumbnails` and stands the target down on exit |
| Consumers | AudienceMonitor (`OutputPanel`, Audience), `EditCanvas` (Selection + box chrome in `overlay`), ThemeEditor centre canvas (Selection + placement overlay), Show cue cards and Bible cards via `SlidePreviewCard` (Thumbnail) |
| Automation | host-assigned `objectName` (existing ones preserved: `audiencePreview` stays the OutputPanel frame); the placeholder text node is named `<host>PreviewNote` when a page needs to assert on it |

### SlidePreviewCard

`src/ui/SlidePreviewCard.qml` (CS.2). Surface plus caption, selection/pending/
live chrome and activation/context signals. A card is a click target, not a
focusable button (cue stepping must keep the keyboard).

| | |
|---|---|
| Inputs | the embedded `SlidePreviewSurface` via the `surface` alias (bound through by the host: target, key, controller, text fallback as `surface.placeholder` with `placeholderVisible`); `caption` (string; the strip fills the room under the picture); `selected`, `live` (booleans — chrome only, the host computes them from acknowledged worker state; a pending go wears a host chip, not the card border); `idleColor`/`hoverColor` |
| Events | `activated()` (primary click), `contextRequested(mouseX, mouseY)` (right click, in CARD coordinates — the host maps it to wherever its menu lives) |
| Adapter | the host decides what `activated()` means: Show presents the cue; Bible selects the generated slide off air; Edit selects the authored slide; Themes selects the view. The card itself never calls a presentation verb |
| Consumers | ShowPage cue tiles, EditorSlideRail authored-slide tiles, Bible generated-slide cards, ThemeEditor Songs/Bible view tiles |
| Automation | `objectName` per host (`operatorCueTile` keeps its `index`/`modelData`-compatible properties for the cue tests); Accessible.name assembled by the host |

### SlidePreviewCollection

`src/ui/SlidePreviewCollection.qml` (ungrouped CS.3, grouped CS.7).
Virtualized responsive preview collection with group boundaries, index
mapping and visible-band reporting — a GridView over a computed visual-CELL
model, so only visible cells have live delegates. The host feeds `source`
(flat, presentation order) and gets back a delegate context of
`{cueIndex, row}` per cell; in grouped mode a group's leftover row columns
become FILLER cells the delegate must render invisible and inert (Show does:
no name, no picture, no clicks). Groups cut on the row's `entryId` —
occurrence identity, never show name, theme or context — and every group
starts at column zero: a group ending mid-row pads the rest of that row,
the next occurrence opens the next row, and a group ending exactly at a row
end forces no blank row. Cues within a group flow and wrap normally; the
collection displays and maps and never reorders cues.

Show draws a full-width separator in the existing gap above each subsequent
service occurrence, without adding source rows or changing the cell mapping.
Selecting a service entry keeps the complete source order and scrolls to that
occurrence; only explicit Library browsing uses a single-show source.

CR.6: fixed-cell column counts follow `cellWidth`, including a host override;
stretched collections use `cardWidth` to choose a column count before dividing
their width. Visible-band geometry includes the actual partial last row and
one full look-ahead row at fractional scroll offsets.

The host asks for thumbnails off the view's ACTUAL window (CS.8): the rows on
screen plus one bounded look-ahead row, filler gaps costing nothing, the
debounced `visibleBandChanged` restarting the host's single coalescing timer
so a scroll burst costs one ask at the destination. A zoom step re-derives
everything from the newly mapped cells — regenerating the cell model restarts
the GridView layout at the top, so the ask follows wherever the view sits and
a scroll back to the bottom prioritizes the last cue. CR.7 retains the full
bounded viewport window and sends successive packets of at most 32 scenes;
wire batching no longer truncates the visible demand.

| | |
|---|---|
| Inputs | `source` (flat array of row maps with `cueKey`/`slideId`/`entryId`… — the host's presentation order); `grouped` (false = uniform flow — Bible; true = every entry occurrence starts a new row — Show); `cardWidth` (operator zoom step); `stretchCells` (divide the view's width among the columns, so the grid fills the page at the zoom step's target width — Show); `cellWidth`/`cellHeight` may be overridden by the host as today |
| Outputs | `layout` — the current generation `{cells, cellOfCue}`; `cueRow(cueIndex)` / `cueRect(cueIndex)` — mapped row number and content-space rect for scroll-to-cue; `scrollCueIntoView(cueIndex)`; `visibleIndexes()` / `visibleCueIndexes()` — the SOURCE indices actually on screen (bottom partial row plus one row of look-ahead, fillers never); `columns`; `delegate` is the GridView's own: it receives the cell wrapper `{cueIndex, row}` — or `{filler: true}`, which the host renders as an invisible, input-less Item |
| Events | `visibleBandChanged()` (debounced — the host asks for thumbnails off this); cue activation/context stay between the host's delegate and its own model (the collection adds no signals of its own) |
| Adapter | hosts keep their own flat presentation-order model (`cueRows` for Show; `scripture.slides` for Bible) and read mapped geometry from the view; the collection never reorders cues and never schedules requests |
| Consumers | ShowPage (grouped, `stretchCells`, entry-grouped), BibleImportPage (ungrouped) |
| Automation | `objectName` per host (`operatorSlideGrid` keeps its role for the cue suite); delegate instances keep `operatorCueTile`, whose `index` property is the CUE index (the view's attached cell index differs from it whenever any group has padded — read the cue index, not the cell) |

### InspectorAdapter

`src/ui/InspectorAdapter.h/.cpp` (CS.4). The read model and write router
above, as a `QML_ELEMENT` in the `OpenShow` module — it travels in the
qml-module SOURCES exactly like `PreviewView` (a dynamic `qmlRegisterType`
into the same URI shadows the static module's own types; the interaction
fixture compiles the same source, so QML resolves it identically in app and
test). Pinned headless by `inspector_adapter_tests` over the REAL
controller: the per-field shape for all four targets, every route landing
on its verb, and every refusal naming itself.

| | |
|---|---|
| Inputs | `document` (DocumentController*), `target` (`{kind, id, view}` above) |
| Outputs | `fieldState(field)` — the shape above; `fields()` — this target's committable spellings |
| Events | `changed()` (documents or cursor moved), `refused(field, reason)`, and the `document`/`target` property signals |
| Centralised | the context's WHOLE-entry merge (read back the entry's own set, replace one member, write once). Since CS.5 this is the ONLY spelling: the Theme Editor's rails ride the adapter, and the page's private merge arithmetic is gone — `copyThemeBox` and the Bible drag both commit `"box"` through it |
| Automation | none — not a visible part; hosts bind it inside their rails |

TE.6: a valid theme's absent Bible context is an editable inherited target,
not an unusable one. Reading never creates a record. The first supported
field edit seeds the compiled typography/box (and inherited context movement,
when present), merges the requested field and writes one undoable record.
A first transition edit uses the same atomic path. ThemeEditor keeps its
Bible controls visible and provides Reset Bible to remove the own record;
placement outlines carry no instructional caption. BoxGeometryControls treats
focus-out on unchanged displayed coordinates as a no-op, including inherited
and fractional resolved boxes.

### AppearanceInspector

`src/ui/AppearanceInspector.qml` (CS.5). Composition of the five style
sections for ONE adapter target — the part takes `adapter` only; the target
lives on the adapter, so browsing a selection moves `target` and every
section re-reads itself. Section order: Background → TextStyle → BoxGeometry
→ TextEffects → Transition (the context rail's box now sits after the type
rows, which is where the reading look is reasoned about).

| | |
|---|---|
| Inputs | `adapter` (InspectorAdapter, required); `worker` (required, threaded to the family picker); `overlayParent` (Item holding the picker's popup); section flags `showBackground`/`showStyle`/`showGeometry`/`showEffects`/`showTransition` (default true — the Bible rail passes `showBackground: false, showEffects: false` because a text context cannot store colours until TV.3: the section is ABSENT, not a rail of disabled rows); `showRevertChips` (default false) + `revertBadgePrefix` (default `"customizeBadge"`) forwarded to the colour rows, the family row and the size/align/spacing rows — the Edit rail's chip tradition (CS.6); effects inherit whole through their Off button and the style-block rows through their Inherit buttons, so no chip joins those |
| Outputs | readonly aliases `backgroundControls`, `styleControls`, `geometryControls`, `effectsControls`, `transitionControls` for the host to pin labels and automation names; `syncAll()` re-reads every section (call it after pinning, since the pins change what a sync draws) |
| Events | none of its own — commits and refusals run through the adapter, and each section re-syncs itself on the adapter's `changed()`/`targetChanged()` |
| Consumers | ThemeEditorPage Songs rail (all five sections over `{kind:"theme"}`) and Bible rail (sparse type + one box + movement over `{kind:"context", view:"scripture"}`); EditPage slide rail (CS.6 — `{kind:"slide"}` with `showGeometry: false` (the page keeps its own `geometryField`/drag story) and `showTransition: false` (a slide has no per-slide movement), `showRevertChips: true`) |
| Automation | every inner control's `objectName` is a string property of the owning section (defaults generic); pages pin the names their tests read (ThemeEditor re-pins its full `themeEditor*` set in `pinRailNames()`) |

### TextStyleControls

`src/ui/TextStyleControls.qml` (CS.5). Family row (embedded
`WorkerFontPicker`), size, alignment, line spacing and — unless
`showStyleBlock: false` — the weight/slant/fit/minimum-size block with the
own/inherited badges and Inherit buttons.

| | |
|---|---|
| Inputs | `adapter` (required), `worker` (required, for the family row), `overlayParent`; `sparse` (empty typed values reset context overrides); `showInheritedValues` (when true, show resolved values and the same three alignment choices as Songs, with reset chips returning fields to inheritance; otherwise show own-or-empty values and the leading `"(theme)"` alignment choice). Font browsing is available in either mode; `showStyleBlock`; `sectionTitle` (the Songs rail titles this section "Typography & effects"); label strings `fontLabel`/`sizeLabel`/`alignLabel`/`spacingLabel`; `fontShowStatus`; `showRevertChips`/`revertBadgePrefix` (a ↺ beside size/align/spacing named `<prefix><Field>`, shown from `canReset`, one click resets; the family row forwards them to its picker; the style-block rows keep their own Inherit buttons instead); one `*ObjectName` string per control (family, browse, status, popup, search, list status, list, row, cancel, size, align, spacing, weight, weightInherit, slant, slantInherit, fit, fitInherit, minSize, minSizeInherit); alias `fontPicker` |
| Outputs | `sync()` — assignment, never binding (the adapter answers are function calls; a mid-edit control must not be yanked by an unrelated change) |
| Events | none of its own — every row commits ONE field through the adapter (`commitOrSync`: a refusal re-syncs from the record); base fields route via the target's own verb, the style block via `setThemeTextStyleField`/`customizeCurrentSlide`; the Inherit buttons and chips `reset` |
| Rules | min-size validates shape here (finite, 1..4096) and the domain keeps the last word; a focus-out whose text equals the record re-reads and writes nothing (`commitNumberIfChanged`, and the sparse variant compares against the resolved value when `showInheritedValues` is on, otherwise the own value); a combo choice always commits, even equal to the resolved value — a deliberate pick that materialises is an edit |
| Consumers | AppearanceInspector (Songs: full block; Bible: `sparse` + `showInheritedValues`, reset chips, no style block) |

### TextEffectsControls

`src/ui/TextEffectsControls.qml` (CS.5). Outline and shadow whole-record
editing with enable/off state, own/inherited badges and the resolved values.

| | |
|---|---|
| Inputs | `adapter` (required); `*ObjectName` strings for the two checks, the two Off buttons, both colour fields and all five sliders |
| Events | commits through the adapter with the TS.4 member spellings (`strokeEnabled`, `strokeColor`, `strokeWidth`, `shadowEnabled`, `shadowColor`, `shadowOpacity`, `shadowOffsetX/Y`, `shadowBlurSigma`); `reset("stroke")` / `reset("shadow")` for the whole-record clear |
| Rules | enable is the record's own materialise (a silent target gains the contract's defaults around the one member — the audience already draws that look); sliders commit on RELEASE only, and not at all when the handle ended where it began; a typed-unchanged colour writes nothing; a target that cannot store effects shows the section disabled, never the Songs record half-written from a Bible rail |

### BackgroundControls

`src/ui/BackgroundControls.qml` (CS.5). The colour rows a base record
stores: text colour, background colour, and the gradient colour whose empty
string is the EXPLICIT flat.

| | |
|---|---|
| Inputs | `adapter` (required); `textColorObjectName`/`backColorObjectName`/`gradientObjectName`; `showRevertChips`/`revertBadgePrefix` (CS.6: a ↺ chip named `<prefix><Field>` beside each colour this target owns, one click hands it back) |
| Events | `commit("textColor"/"backgroundColor"/"backgroundGradient", …)` through the adapter (each commit restates the WHOLE base record — one omitted field cannot silently flatten a gradient); a focus-out typed equal to the record re-reads and writes nothing (`commitIfChanged`) |
| Note | `""` gradient stays explicit flat, distinct from inherit — these base fields never inherit (the adapter refuses the reset and says so). Background IMAGE and FIT are not appearance fields: they are slide/service props the host owns (Edit keeps its `backgroundCombo`/`backgroundFitCombo` rows writing `setSlideBackground`/`setSlideBackgroundFit` beside the shared part) |

### BoxGeometryControls

`src/ui/BoxGeometryControls.qml` (CS.5). The shared X/Y/W/H rows: four
fields that commit as ONE unit, a removal button, and an optional extra
action (the Bible rail's "Theme's lyric box" borrow).

| | |
|---|---|
| Inputs | `adapter` (required); `label` (the rail names its rectangle); `clearLabel` ("No placement" / "No box"); `extraActionLabel` ("" hides the button); `showStatus` (Songs states whether the target places at all); `statusOwnedText`/`statusSilentText`; `xObjectName`/`yObjectName`/`wObjectName`/`hObjectName`/`clearObjectName`/`extraObjectName`/`statusObjectName` |
| Events | the fields commit `"box"` through the adapter (routes to `setThemePlacement`/`clearThemePlacement`, the merged context box retiring a stored `inset` in the same edit, or the slide's `setTextBoxGeometry` in CS.6); `extraClicked()` forwards the borrow to the host; `sync()` |
| Rules | validation is SHAPE only — four finite numbers with w/h ≥ 1 (this replaces the context rail's earlier `w > 0` spelling: a box thinner than a pixel cannot be placed either way, and the domain still has the last word); any refusal or invalid typing puts every field back where the record says; the page's drag gesture commits the same `"box"` verb, so drag and typed path are one and the same undo |

### TransitionControls

`src/ui/TransitionControls.qml` (CS.5). The movement rows: the kind picker,
both durations, and — for targets that can be silent — the Inherit entry,
the own/inherited badge and the source sentence naming the seat whose
numbers are on show. One part serves the Songs rail, the Bible rail and the
Settings service section.

| | |
|---|---|
| Inputs | `adapter` (required); `allowInherit` (model `[Inherit, Cut, Crossfade]` vs the service's `[Crossfade, Cut]`); `showKindHint` (Settings explains the choice inline beside the picker); `showOwnBadge`/`showSource`; `silentSuffix` (appended to the source sentence when the target is silent — "a theme lower in a wearing slide's chain answers first" / "the theme carrying this view answers before the service"); `wordsLabel`/`backLabel`; `labelPixelSize` (12 on the Settings page); `sectionTitle` ("" hides it — Settings keeps its own page heading); `note` + `noteObjectName` (the host's standing note, verbatim); `kindObjectName`/`inheritObjectName`/`wordsObjectName`/`backObjectName`/`sourceObjectName` |
| Events | commits `"transition"` as ONE whole record through the adapter (`setThemeTransition` / `setTextContextTransition` / `setServiceDefaultTransition`); the Inherit entry and button both WRITE NOTHING — the record is removed, and only against a record that exists |
| Rules | QML validates SHAPE only (digits, no sign, no decimal); the 0..10000 band stays `Documents::validTransitionOptions` and a refusal re-reads the record; a Cut DISABLES the durations without erasing them; Cut and Crossfade each write a whole record seeded from the numbers on show; an unchanged commit writes nothing and costs no undo |

### WorkerFontPicker

`src/ui/WorkerFontPicker.qml` (CS.5). The family row: a typed name that
always works, the audience worker's own family list behind Browse, and the
missing-font note.

| | |
|---|---|
| Inputs | `worker` (required — the family source), `adapter` (required), `field` (today always `"fontFamily"`); `sparse` (an empty typed family resets its override); `showInheritedValues` (display and validate the resolved family, without creating an override on unchanged focus-out); `showStatus`; `showBrowse`; `label`; `overlayParent` (the popup is opened against the PAGE, so the rail's scroll cannot clip it); `editable` (a rail without a selected target types nothing); `showRevertChip`/`revertBadgePrefix` (the Edit rail's ↺ beside the family field, named `<prefix>FontFamily`, shown from `canReset`); `*ObjectName` strings for field/browse/status/popup/search/list status/list/row/cancel |
| Outputs | `sync()`; `familyChosen(name)` when the list answers |
| Rules | typed names remain usable with no worker at all; a typed focus-out trimmed and equal to the record re-reads and writes nothing; Browse is ENABLED only when the worker named `font.list` (a disconnected rail gets a disabled button and a status line saying why, never a list of the operator's desktop fonts the show would silently fall back from); the popup lists ONLY the worker's families and closes when the host page hides; a missing-family warning requires a COMPLETED worker list; the requested name is never rewritten by a fallback verdict |
| Consumers | `TextStyleControls` (the Songs rail's row; the Bible rail's sparse row) |

### BibleImportWorkspace (`BibleImportPage.qml`)

Current workflow (BIU.1, superseding CS.3's two-preview layout): reference and
translation lookup, one generated `SlidePreviewCollection`, then Add/Cancel.
Every fetched verse is included; the verse-checkbox list, All/None, duplicate
Selection canvas, separate Retry button and standing help paragraph are removed.

| | |
|---|---|
| Inputs | `bible` and `scripture` session context properties |
| Lookup | Reads the current reference field before sending; a new lookup replaces the whole passage. Editing the reference retires the old request/verses/attribution so old content cannot be added with the new label. Lookup also retries failures. |
| Preview ownership | Thumbnail keys remain `scripture:<planGeneration>:<index>`. The page disables the session's Selection rendering and never calls `previewSlide`; cards display the generated plan without a selected-slide state. Leaving stops the thumbnail ask. |
| Add | Adds the complete fetched passage as one undoable schedule item. Pending measurement or a field/reference mismatch disables Add. Existing notes, attribution, word conservation and double-Add protection remain. |
| Status | Retains online/error messages and a concise slide-ready count; no standing rendering explanation. |

### AudienceMonitor (inside existing `OutputPanel.qml`)

The live picture/status plus the existing per-layer clears. After CS.2 the
picture is a `SlidePreviewSurface` bound explicitly to `Audience`; the
clears, transport and audio areas stay in `OutputPanel` OUTSIDE the surface
— output-specific operations are never part of a reusable preview part.
Existing automation names (`audiencePreview`, `previewOfflineNote`,
`clearControls`, `clearAllButton`, …) are preserved.

## Migration map (what each leaf moves)

| Leaf | Moves |
|---|---|
| CS.2 | Extract `SlidePreviewSurface` + `SlidePreviewCard`; migrate OutputPanel `previewBox`, `EditCanvas` stage, ThemeEditor `previewFrame` + `ViewTile`, ShowPage tile delegate; add fitting/isolation/overlay/placeholder coverage |
| CS.3 | `SlidePreviewCollection` ungrouped mode; Bible cards + selected in-page canvas; keyed plan-generation thumbnails; wording corrections |
| CS.4 | `InspectorAdapter` read model + commit/reset routing documentation; centralize the context merge in the adapter/controller |
| CS.5 | Extract `AppearanceInspector`, `TextStyleControls`, `TextEffectsControls`, `BackgroundControls`, `BoxGeometryControls`, `TransitionControls`, `WorkerFontPicker`; migrate ThemeEditor Songs/Bible rails and Settings' transition section |
| CS.6 | EditPage's appearance rail replaced by an `AppearanceInspector` over `{kind:"slide"}` (`showGeometry`/`showTransition` off, `showRevertChips` on). Renames the tests see: the old left/centre/right trio is one combo now, pinned `themeAlignCombo` (the trio names retired); the weight/slant/fit/min-size/stroke/shadow rows stay named `editWeight(+Inherit)`, `editSlant(+Inherit)`, `editFit(+Inherit)`, `editMinSize(+Inherit)`, `editStroke*`, `editShadow*` — pinned onto the shared parts by `pinEditNames()`, same names, new parts. The old hand-rolled `commit()`/`valueOf()`/badge rows retired into the parts' `*ObjectName` pins and `<prefix><Field>` chips; `revertableNames` grew to the advanced fields and placement, so Revert-customizations covers advanced-only changes |
| CS.7 | DONE (as-built in the parts above): ShowPage on `SlidePreviewCollection` `grouped: true` — groups cut on `entryId`, every group opens at column zero, leftover row columns are inert fill. Scroll-to-cue follows `scrollCueIntoView`/`cueRect`; the ask window follows `visibleCueIndexes()` (full bounded window, bottom-prioritized at the end of the order; CR.7 sends successive packets of at most 32). Delegate context is now `{cueIndex, row}`; `operatorCueTile` keeps a cue-equal `index` property; Bible's host input `model` became `source`. Keyboard, live/pending identity and the flat cue order unchanged |
| CS.8 | DONE: the ask and the scroll run entirely on grouped mapped geometry. `visibleCueIndexes()` is the window — viewport rows plus ONE bounded look-ahead row, filler gaps never — and the host's debounced `visibleBandChanged` keeps one coalescing timer: five grouped scroll jumps inside the window ask for nothing and the burst lands one ask describing the destination. Bottom case: the clamped window ends on the order's last cue and the capped band anchors there (cap cutting edge pinned in UI.3c over grouped rows); zoom case: every thumb step re-derives the band from the NEW mapped cells, the requested pixel size follows the zoom, and a group head plus a deep cue scroll contained at all five thumb sizes. Pinned by the CS.8 suite block over a 38-cue five-entry order |
| CS.9 | DONE: the seams are walked, not asserted per-part. One UI tour writes a theme size on the Themes host and reads it back in the Edit host's resolved field; customizes the slide (chip wakes and announces "Revert fontSize customization", the theme row stays 48); Remove-customizations lands back on the theme value; Tab walks on, Escape releases without committing, leaving the page takes the field and commits as one ordinary edit; the rail's first and last controls reach the viewport at 1000×680; the hidden Show page's `bandSent` counter moves for none of the tour's churn while a shown page catches up; browsing (theme rows, view tiles, pages, a cue click) moves no document, history or output byte; the walk-back returns the record to its starting shapes. Persistence of the records these controls write stays pinned by `document_controller_tests` save/load round-trips, and the remaining TV/TA leaves (TV.3/TV.4a/TV.4b/TV.5/TA.1) are recorded open as the FO gate |


## Remediation acceptance (CR.9)

`control_set_ui_tests` covers shared-card containment, populated verse geometry
and clicks, responsive lookup, inherited inspector editing, collection mapping
and narrow header geometry. Its optional `OPENSHOW_CONTROL_CAPTURE` mode loads
production Main QML with a scripted Bible provider and the real rendering worker;
it captures populated Show, Bible, Edit and both Themes views at three sizes.
Run separately at scales 1, 1.5 and 2. See the [acceptance record](../reviews/CS-remediation-acceptance-2026-11-05.md)
for commands, limits and results. The old `OPENSHOW_WORKSPACE_CAPTURE` mode is
shell/empty-state evidence, not a substitute for this populated tour.

## Shared song Library and clipboard import (SL / SI)

| Part | Contract |
|---|---|
| `LibraryBrowser` (`libraryBrowser`) | Hosts Songs (catalog) and Presentations (service shows + catalog records categorised as presentations). Owns `librarySearchField`, `librarySongList`, `collectionLibraryList`, both category headers as Move drop targets, `catalog:<id>`/`show:<id>` drag sources, the song context menu (`librarySongMenu`) and rename dialog (`libraryRenameSongDialog`). Focused Delete confirms permanent Library removal. PowerPoint files drop on the Presentations header; songs/presentations drag into the service. The redundant Add/Import/New/Remove buttons and manual-creation dialog are removed. Browsing never presents. |
| `CurrentServicePanel` (`currentServicePanel`) | The one running order (`collectionPlaylistList`, `collectionPlaylistRow`) with reorder/remove handles, `serviceRemoveSelectedButton`, scrolled insertion gaps, `catalog:`/`show:`/`entry:` drop handling and bounded edge autoscroll. No service picker in this milestone. Clicking a cue selects it and browses its show; it never presents. |
| `ListSelection` | Shared stable-key selection model: ordinary click replaces, Ctrl toggles, Shift extends from the anchor, Ctrl+Shift adds a range, Ctrl+A selects all filtered rows in expanded branches (including scrolled-off rows). Library keys are typed `catalog:`/`show:` identities; service keys are occurrence-specific entry IDs. Filtering/collapsing prunes hidden selections. |
| `SongImportPage` (`songImportPage`) | Off-air import page over `SongImport`: `songPasteButton` (normal clipboard only), `songTitleField`, `songCcliField`, `songAuthorsField`, `songCopyrightField`, `songSlideCollection`/`songSlideCard` (thumbnail target, keys `song:<plan>:<index>`), `songAddToLibraryButton`, `songCancelButton`, `songImportBackButton`. It owns thumbnails only (no Selection canvas), stands them down when hidden, and writes nothing before Add. |
| `SongPlanner` (session) | Not a visual part: the shared non-blocking measurement runner (`text.measure` cache, one batch in flight, latest-ask-wins) used by both the import page and the service bridge. Exposes no persistence. |
| `SongImport` (session) | Draft → measuring → ready/error → committed/cancelled; title and CCLI/author/copyright fields; `addToLibrary` is its only durable write. |
| `ServiceCatalogBridge` (session) | `addToService`/`refreshFromLibrary`/`refreshAllLinked`/`renameInService`; prepares a catalog song into one referenced service show and reacts to `CatalogController::itemChanged`/`itemRemoved`. Never edits the catalog and never presents. |

The Library search uses `PanelField.fixedPlaceholderText` for a stationary hint;
Ctrl+F focuses it without changing the service. The redundant toolbar Search
control is removed. More → Import song opens the same clipboard page as the
Library button, and completion/cancel returns to the originating page.
`PanelButton` leaves sufficient content height for labels on compact 24 px rows.
The open Media drawer leaves at least 400 px for the preparation area; the
Library reserves one full row in each expanded category and the service keeps
its own scrolling viewport. Compact action widths include room for their labels.

Delete acts only when a list itself has keyboard focus. Text fields keep their
native Delete and Undo. Remove buttons use the same selection batches as Delete.
Service removal validates all entry IDs and applies one history step. Library
catalog removal writes one durable batch before publishing notifications; a
failed write keeps both records and selection. Authored presentation removal is
one document edit: referenced shows become service-only, while unreferenced
shows are deleted. Detached catalog copies stay out of the Library. Prepared
service cues and slide content survive Library removal and restart.

Ctrl+Z/Ctrl+Y/Ctrl+Shift+Z route to the focused Library/service list on Show as
well as Edit. A mixed Library removal spans catalog and document histories;
its immediate undo/redo visits both stores. It is not a transaction across two
files. Adding or dropping into the service hands focus to the destination.
