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

Transparent Selection/Thumbnail pixels are painted over a cached muted
checkerboard by PreviewView. The worker frame and scene stay alpha; audience
mirrors and opaque media keep their actual composition. Letterbox regions stay
black. Song sections are marked on the card strip (Verse, Chorus, …); the Show
grid and the Edit rail lead that strip with the slide number and leave the
item's name to the band or heading above, while the clipboard import preview
keeps "Section – Song name".

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

Song sections use muted caption-strip colours: verse blue, chorus/refrain teal,
bridge purple, pre-chorus indigo, post-chorus green, opening slate and ending
amber. Consecutive section cards retain their coloured strip and lyric picture
while suppressing duplicate caption text. Live red and selection borders retain
their existing priority. Canonical labels and palette live in the shared card;
Show, Edit and clipboard import supply section boundaries from model order.

A host may also pass `ordinal`, a slide number that leads the strip on every
card. Show uses it so a strip reads "3  Verse 1" at a section start and just
"3" on a continuation — the item's own name lives on the group band above the
row, never repeated per slide. The Edit rail does the same behind its own
heading; the clipboard import preview keeps naming the song it is previewing.

| | |
|---|---|
| Inputs | the embedded `SlidePreviewSurface` via the `surface` alias (bound through by the host: target, key, controller, text fallback as `surface.placeholder` with `placeholderVisible`); `caption` (string; the strip fills the room under the picture); `ordinal` (optional slide number shown first on every card); `sectionLabel`/`showSectionLabel`/`sectionPrefix`/`sectionSuffix` (optional section caption, consecutive-heading gate and title affixes; neutral default preserves ordinary captions), `selected`, `live` (booleans — chrome only, the host computes them from acknowledged worker state; a pending go wears a host chip, not the card border); `idleColor`/`hoverColor` |
| Events | `activated()` (primary click), `contextRequested(mouseX, mouseY)` (right click, in CARD coordinates — the host maps it to wherever its menu lives) |
| Adapter | the host decides what `activated()` means: Show presents the cue; Bible selects the generated slide off air; Edit selects the authored slide; Themes selects the view. The card itself never calls a presentation verb |
| Consumers | ShowPage cue tiles, EditorSlideRail authored-slide tiles, Bible generated-slide cards, ThemeEditor Songs/Bible view tiles |
| Automation | `objectName` per host (`operatorCueTile` keeps its `index`/`modelData`-compatible properties for the cue tests); Accessible.name assembled by the host |

### CardChrome

`src/ui/CardChrome.qml` (MB.8). The shared card surface: idle/hover/selected
colour, border width and radius, and the default content slot. It is
display-only chrome; the host overlays its own MouseArea, so click, drag and
context semantics stay with the wearer. `MediaCard` and the Library's song and
presentation rows wear it, so the selection and hover colours cannot drift.

| | |
|---|---|
| Inputs | `selected`, `hovered` (booleans); `idleColor`, `hoverColor`, `selectedColor`, `idleBorder`, `selectedBorder`; `borderWidth`, `selectedBorderWidth`, `radius`; the default `content` slot |
| Events | none — the host owns the pointer |
| Consumers | `MediaCard`; the Library's song and presentation rows |
| Automation | the host's row names; CardChrome carries no objectName of its own |

### MediaCard

`src/ui/MediaCard.qml` (MB.1–MB.5). One media asset as a preview card in the
Media Bin grid: the picture where the file has one, our own kind artwork
otherwise, the name, kind and one notice. It is a selection target and a
drag-source host, never the owner of a document verb.

Artwork is drawn, not shipped as per-kind bitmaps: a neutral camera silhouette
tinted into four source colours with the numeral 1–4 over it (no NDI mark), a
record/turntable glyph for audio, a film-frame mark for video, a document mark
for Other. No kind carries a play button: a card is a library object, not a
transport control. The camera drawing is `CameraArtwork`, worn by the bin card and by an
off-air camera cue in the service preview, so a camera looks like itself
everywhere. The model caps camera assets at four (`kMaxCameraAssets`), so the
numeral always has a slot; a fifth add is refused with a sentence.

A camera asset also carries `cameraMirror` (ND.9): a phone's FRONT camera can
deliver a picture mirrored against the operator's view of the room. The NDI
picker asks once at add time (the same undo removes the camera and its answer),
and the card's menu flips it later; the flag rides the scene to the worker,
which flips the live frame about the canvas' vertical axis.

The card drags `media:<assetId>` through the shared `DragFeedback`. Dropping it
on a Show cue sets that cue's slide **background media** in one undo; dropping
an AUDIO card on a cue adds it to that **show's audio layer** instead (the cue
keeps its pictures, so a lyric chart plays while its words step); dropping it
on the running order's list creates a cue of its own — one schedule-owned show
with a single slide and, for audio, no picture at all. A rendered deck page
(`Asset::deckPage`) is stored but never a bin row: the deck is a unit and its
pages are hidden from the grid and its collection counts.

| | |
|---|---|
| Inputs | `modelData` (the asset row: id, name, kind, sourcePath, cameraSource, cameraMirror, exists); `kindLabel`; `notice`; `selected`; `cameraNumber` (1–4, creation order) |
| Events | `activated()` (primary click), `contextRequested(mouseX, mouseY)` (right click, in CARD coordinates), `dragPayload` (`media:<assetId>`) |
| Adapter | the bin sets `selectedAssetId` on activation, opens its shared menu on context, and the Show tile's DropArea calls `setSlideBackgroundFor` |
| Consumers | the Media Bin grid; the Library's row cards share `CardChrome`, not this whole card |
| Automation | `mediaBinRow` (delegate root), `mediaRowName`, `mediaRowKind`, `mediaRowNotice`, `dragPayload`, `cameraNumber` |

### CameraArtwork

`src/ui/CameraArtwork.qml` (MB.5 drawing, ND.9 sharing). The Media Bin's own
camera glyph — neutral silhouette, per-source tint and the camera's numeral —
extracted so the bin card and the service preview cannot drift apart. It draws
nothing else: the host sizes it and decides when it stands in for a picture.

| | |
|---|---|
| Inputs | `number` (1–4, the bin's creation-order camera number) |
| Events | none |
| Adapter | `MediaCard` shows it for a camera asset; `ShowPage` shows it over an off-air camera cue (a shown camera has a live frame instead) |
| Automation | host-assigned objectName (`cueCameraArtwork` on the service cue) |

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

Show draws a full-width name band above each service occurrence, in the row
space the grid reserves for it, without adding source rows or changing the cell
mapping. At a group start the band carries the running-order item's name on one
line (and its playlist section, when it has one); between that group's own rows
the same space is simply air. Show overrides the host `cellHeight` so one row is
its header band, the card and the gap under it. Selecting a service entry keeps
the complete source order and scrolls to that occurrence; only explicit Library
browsing uses a single-show source.

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
The text-layout pair (TL.5) is a leaf on every target and the ONE part of the
shared style block a context already stores, so `verticalAlign`/`allCaps`
answer with the typography's editable inheritance rather than TV.3's
"cannot store" badge; the whole-entry merge carries an untouched pair across a
one-field edit, and the domain's sparse set — not a QML name list — decides
whether the slide's Remove customizations button is live.
A first transition edit uses the same atomic path. ThemeEditor keeps its
Bible controls visible and provides Reset Bible to remove the own record;
placement outlines carry no instructional caption. Since BX.3 the box is
edited only by the canvas gesture; its reset lives on the box context menu,
so there is no numeric field to treat a focus-out as a no-op.

### AppearanceInspector

`src/ui/AppearanceInspector.qml` (CS.5). Composition of the style sections
for ONE adapter target — the part takes `adapter` only; the target lives on
the adapter, so browsing a selection moves `target` and every section
re-reads itself. Section order: Background → TextStyle → TextEffects →
Transition. The numeric BoxGeometry section is gone (BX.3): the box is placed
by dragging the sample/slide directly, and placement reset is on the box
context menu.

| | |
|---|---|
| Inputs | `adapter` (InspectorAdapter, required); `worker` (required, threaded to the family picker); `overlayParent` (Item holding the picker's popup); section flags `showBackground`/`showStyle`/`showEffects`/`showTransition` (default true — the Bible rail passes `showBackground: false, showEffects: false` because a text context cannot store colours until TV.3: the section is ABSENT, not a rail of disabled rows); `showRevertChips` (default false) + `revertBadgePrefix` (default `"customizeBadge"`) forwarded to the colour rows, the family row and the size/align/spacing rows — the Edit rail's chip tradition (CS.6); effects and the style block carry their own always-present, dim-until-resettable ↺ chips, so the forwarded prefix does not name those |
| Outputs | readonly aliases `backgroundControls`, `styleControls`, `geometryControls`, `effectsControls`, `transitionControls` for the host to pin labels and automation names; `syncAll()` re-reads every section (call it after pinning, since the pins change what a sync draws) |
| Events | none of its own — commits and refusals run through the adapter, and each section re-syncs itself on the adapter's `changed()`/`targetChanged()` |
| Consumers | ThemeEditorPage Songs rail (colours, type, effects, movement over `{kind:"theme"}`) and Bible rail (sparse type + movement over `{kind:"context", view:"scripture"}`); EditPage slide rail (CS.6 — `{kind:"slide"}` with `showTransition: false` (a slide has no per-slide movement) and `showRevertChips: true`). The box is not an inspector section: it is the canvas gesture, and its reset is on the box context menu (BX.3) || Automation | every inner control's `objectName` is a string property of the owning section (defaults generic); pages pin the names their tests read (ThemeEditor re-pins its full `themeEditor*` set in `pinRailNames()`) |

### TextStyleControls

`src/ui/TextStyleControls.qml` (CS.5). One family-and-size row (embedded
`WorkerFontPicker` plus the size field where Browse used to sit), horizontal
alignment, the vertical anchor, All caps, line spacing and — unless
`showStyleBlock: false` — the bold/italic/underline switches, fit and minimum
size. The family and size labels are silent by default (review 2026-11-10):
the typeface name and the number speak for themselves.

The layout rows are the text-layout group's (TL.5) and sit in a fixed order on
every host: horizontal alignment, then **Vertical** (exactly three in-repository
paragraph marks — top/centre/bottom — in a `SegmentedControl` with
`glyphStyle: "vertical"`, never a platform icon font), then the **All caps**
checkbox, then line spacing. A theme, the Bible view and a slide all answer for
the pair (a text context is the ONE style pair it stores until TV.3), so the
same rows read the resolved value, show the own/inherited dot and grow the same
reset chip. Each icon has an accessible name and a tooltip and is reachable by
Tab; Space/Return activate it as well as a click. `allCaps` is a strict
boolean: unchecking it over an inherited `true` authors an explicit `false`
(the answer that keeps the words in their original case), and only the reset
chip removes the override.

The style reads VISUALLY (review 2026-11-10): **B / I / U** are three
glyph switches whose own treatment is the sample (bold B, italic I,
underlined U); a switched-on field is accent-filled from the model's
RESOLVED answer. A ↺ chip appears beside the switch this target OWNS, so
the row shows both what it draws and what it changed. The contract still
stores the full 100..900 weight scale; the switch authors its two poles
(700 on, 400 off). Alignment and fit stay `SegmentedControl`s; underline is
the scene contract's newest style leaf (`fontUnderline`, service format 12 /
scene 6).

| | |
|---|---|
| Inputs | `adapter` (required), `worker` (required, for the family row), `overlayParent`; `sparse` (empty typed values reset context overrides); `showInheritedValues` (when true, show resolved values and the same three alignment choices as Songs, with reset chips returning fields to inheritance; otherwise show own-or-empty values and the leading `"(theme)"` alignment choice). Font browsing is available in either mode; `showStyleBlock`; `sectionTitle` (the Songs rail titles this section "Typography & effects"); label strings `fontLabel`/`sizeLabel` (both `""` by default — the family+size row needs no heading)/`alignLabel`/`spacingLabel`; `fontShowStatus`; `showRevertChips`/`revertBadgePrefix` (a ↺ beside size/align/spacing named `<prefix><Field>`, shown from `canReset`, one click resets; the family row forwards them to its picker; the style-block and layout rows keep their own pinned `*InheritObjectName` chips); one `*ObjectName` string per control (family, status, popup, search, list status, list, row, cancel, size, align, verticalAlign, allCaps, spacing, weight, weightInherit, slant, slantInherit, underline, underlineInherit, fit, fitInherit, minSize, minSizeInherit, verticalAlignInherit, allCapsInherit); alias `fontPicker` |
| Outputs | `sync()` — assignment, never binding (the adapter answers are function calls; a mid-edit control must not be yanked by an unrelated change) |
| Events | none of its own — every row commits ONE field through the adapter (`commitOrSync`: a refusal re-syncs from the record); base fields route via the target's own verb, the style block and the layout pair via `setThemeTextStyleField`/`customizeCurrentSlide` (a context's `verticalAlign`/`allCaps` via its own context merge); bold commits `fontWeight` 700/400, italic `fontStyle` italic/normal, underline `fontUnderline` true/false, the vertical row `verticalAlign` top/center/bottom, All caps `allCaps` true/false; the ↺ chips `reset` |
| Rules | min-size validates shape here (finite, 1..4096) and the domain keeps the last word; a focus-out whose text equals the record re-reads and writes nothing (`commitNumberIfChanged`, and the sparse variant compares against the resolved value when `showInheritedValues` is on, otherwise the own value); a segmented choice always commits, even equal to the resolved value — a deliberate pick that materialises is an edit |
| Consumers | AppearanceInspector (Songs: full block; Bible: `sparse` + `showInheritedValues`, reset chips, no style block but the layout pair and the typography a context stores) |

### SegmentedControl

`src/ui/SegmentedControl.qml` (review 2026-11-10). A small enumeration
shown as a row of joined buttons, so the active choice is visible at a
glance instead of hidden behind a closed combo.

| | |
|---|---|
| Inputs | `model` (strings or `{value, label}` objects); `currentIndex`; `glyphStyle` (`"align"` draws the three horizontal paragraph marks, `"vertical"` the three vertical-anchor marks); `italicValues` (slants an option whose value is `"italic"`); the former sparse `"(theme)"` entry is shown as "Theme" |
| Outputs | `count`, `currentValue`, `labelAt(i)`/`valueAt(i)`; each segment is named `<objectName>Segment<index>` and carries `Accessible.name`/`Accessible.checked`, so assistive tech and fixtures address the exact choice |
| Events | `activated(index)` — a click or a keyboard activation only; a programmatic `currentIndex` assignment (a sync from the record) never emits it, exactly as a ComboBox does not. Segments take focus by Tab (`activeFocusOnTab`) and Space/Return call `activate(i)`, which is the same path a click takes |
| Consumers | TextStyleControls (horizontal alignment, vertical anchor, slant, fit) |

### TextEffectsControls

`src/ui/TextEffectsControls.qml` (CS.5). Outline and shadow whole-record
editing with enable, a visual own/inherited state and the resolved values.
The state reads as a dot beside each section (accent while the target owns
the record) and a ↺ chip that keeps its place and lights up only while
there is a record to hand back; each colour field wears a live swatch of
the value the audience draws (review 2026-11-10).

| | |
|---|---|
| Inputs | `adapter` (required); `*ObjectName` strings for the two checks, the two reset chips, both colour fields and all five sliders |
| Picker | each colour swatch opens Qt's own `ColorDialog` (`QtQuick.Dialogs`, in-app dialog); accepting commits the chosen hex through the same member verb |
| Events | commits through the adapter with the TS.4 member spellings (`strokeEnabled`, `strokeColor`, `strokeWidth`, `shadowEnabled`, `shadowColor`, `shadowOpacity`, `shadowOffsetX/Y`, `shadowBlurSigma`); `reset("stroke")` / `reset("shadow")` for the whole-record clear |
| Rules | enable is the record's own materialise (a silent target gains the contract's defaults around the one member — the audience already draws that look); sliders commit on RELEASE only, and not at all when the handle ended where it began; a typed-unchanged colour writes nothing; the swatch previews only parseable hex and leaves an unparseable value to the text field; a target that cannot store effects shows the section disabled, never the Songs record half-written from a Bible rail |

### BackgroundControls

`src/ui/BackgroundControls.qml` (CS.5). The colour rows a base record
stores: text colour for presentation themes, plus background colour and gradient
for authored slide targets. Each field wears a live swatch of the value
the audience draws (review 2026-11-10). The two backdrop rows are hidden for
themes; their adapter refuses backdrop writes. A slide gradient whose empty
string is explicit flat stays distinct from inheritance.

| | |
|---|---|
| Inputs | `adapter` (required); `textColorObjectName`/`backColorObjectName`/`gradientObjectName`; `showRevertChips`/`revertBadgePrefix` (CS.6: a ↺ chip named `<prefix><Field>` beside each colour this target owns, one click hands it back) |
| Picker | each swatch opens Qt's own `ColorDialog` (`QtQuick.Dialogs`, in-app dialog); accepting commits the chosen hex through the same field verb, and a colour typed exactly as drawn still writes nothing |
| Events | `commit("textColor"/"backgroundColor"/"backgroundGradient", …)` through the adapter (each commit restates the WHOLE base record — one omitted field cannot silently flatten a gradient); a focus-out typed equal to the record re-reads and writes nothing (`commitIfChanged`) |
| Note | `""` gradient stays explicit flat, distinct from inherit — these base fields never inherit (the adapter refuses the reset and says so). Background IMAGE and FIT are not appearance fields: they are slide/service props the host owns (Edit keeps its `backgroundCombo`/`backgroundFitCombo` rows writing `setSlideBackground`/`setSlideBackgroundFit` beside the shared part) |

### BoxTransform

`src/ui/BoxTransform.qml` (BX.1). The ONE geometry and hit-testing
implementation both canvases consume — the Edit canvas and the Theme Editor's
Songs/Bible placement overlay. Pure functions over the authored 1920×1080
canvas and the fitted preview's `zoom`, so the two pages cannot keep two ideas
of where a corner is or which edges a drag moves. A `QtObject`, instantiated as
an invisible child by each page; it holds no document or history state.

| | |
|---|---|
| Inputs | none; `canvasWidth`/`canvasHeight` (1920×1080 by contract), `minimumSize` (20 authored units), `handleRadius` (8 logical px) are read-only |
| Functions | `snapCentre(x, y, w, h, tolerance)` — keep the size, pull a near axis exactly onto 960/540; `cornerAt(box, px, py, zoom, radius)` — `"tl"`/`"tr"`/`"bl"`/`"br"`/`""`, nearest centre wins when a tiny box overlaps two; `inside(box, px, py, zoom)`; `move(start, dx, dy)` — preserved size, clamped inside the canvas; `resize(mode, start, dx, dy, minimum)` — moves the TWO edges the corner owns, fixes the opposite corner, clamps each moving edge against it and the canvas; `cursorFor(mode)` — `SizeFDiagCursor`/`SizeBDiagCursor`/`SizeAllCursor`/`ArrowCursor` |
| Rules | a corner drag can never flip the item or change its stack order; the tolerance is received in authored units the CALLER derives from the screen-relative 6 px, so aiming feels the same at every zoom; off-canvas legacy geometry is left alone until a corrective drag; `resize` is the correction the reviewed arithmetic lacked (bottom-right was inert, top-right/bottom-left moved one axis) |
| Gesture areas | both hosts extend their pointer surface one `handleRadius` past the fitted content and map coordinates back by the same inset, so the outer half of a corner at a slide edge is live; the target is captured by identity at press and the release writes only while that identity holds and the rounded rectangle changed (BX.2) |

### Box context menu

The box's non-numeric placement actions (BX.3), now that the numeric geometry
section is gone. Both menus open on right-click over the box and on
Menu/Shift+F10, and neither creates a record or a history entry merely by
opening.

| | |
|---|---|
| Edit (`editBoxMenu`) | **Reset box placement** — enabled only while `slideAppearance.placement.customized` is true; calls `revertCurrentSlideField("placement")`, the same verb the removed Follow-theme button used |
| Theme Editor (`themeEditorBoxMenu`) | **Reset box placement** — the Songs view clears the theme's own placement (`themeAdapter.reset("placement")`); the Bible view removes the context's box/inset while the adapter merge preserves every sibling field (`contextAdapter.reset("box")`). Enabled only when that view states an own box |
| Theme Editor, Bible only | **Use theme's box** — commits the theme's resolved rectangle into the context through the existing merge (`copyThemeBox`), preserving typography, layout pair, movement and every other view field |
| Rules | keyboard-reachable (Menu/Shift+F10, arrow/Enter in the menu); a reset is at most one undo step; a secondary authored item keeps its normal undo path and offers no reset |

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

`src/ui/WorkerFontPicker.qml` (CS.5, revised 2026-11-10). The family row:
clicking the typeface name opens a normal NON-MODAL drop-down attached
under the field, listing the audience worker's own families (`canList`),
with a trailing chevron as the affordance. The list filters as you type,
clicking away or Escape closes it, and it never takes the page hostage.
There is no separate Browse button. A worker that cannot enumerate leaves
the field typeable — the path that always works with the worker gone — and
the positive "installed on the audience's machine" sentence is retired, so
only a real problem (checking, missing, unsupported) speaks.

| | |
|---|---|
| Inputs | `worker` (required — the family source), `adapter` (required), `field` (today always `"fontFamily"`); `sparse` (an empty typed family resets its override); `showInheritedValues` (display and validate the resolved family, without creating an override on unchanged focus-out); `showStatus`; `label` (`""` hides the heading); `allowOpen`; `overlayParent` (the popup is opened against the PAGE, so the rail's scroll cannot clip it); `editable` (a rail without a selected target types nothing); `showRevertChip`/`revertBadgePrefix` (the Edit rail's ↺ beside the family field, named `<prefix>FontFamily`, shown from `canReset`); `*ObjectName` strings for field/status/popup/search/list status/list/row |
| Outputs | `sync()`; `canList` (worker enumerates AND the target is editable); `familyChosen(name)` when the list answers |
| Rules | typed names remain usable when no list answers; a typed focus-out trimmed and equal to the record re-reads and writes nothing; when `canList` the field is read-only and a click opens the drop-down (the chevron says so), so a disconnected rail never offers a list of the operator's desktop fonts the show would silently fall back from; the popup lists ONLY the worker's families, is non-modal and closes on press-outside/Escape as well as when the host page hides; a missing-family warning requires a COMPLETED worker list; the requested name is never rewritten by a fallback verdict |
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
picture is a `SlidePreviewSurface` bound explicitly to `Audience`; the clears
and the transport stay in `OutputPanel` OUTSIDE the surface — output-specific
operations are never part of a reusable preview part. The transport governs
whatever cue is on air, video or audio alike: Play/Pause, Loop, seek. The one
master **audio mute** lives in the audience header above the monitor
(`masterMuteButton`, `media.mute` source `all`), not in the transport and not
per source: it silences the background's embedded audio and the show track
together, and releasing it restores the acknowledged mix. The rows under the
transport are volumes only — each labelled "<source> volume" with its
percentage — so there is no second mute checkbox to read. The old separate
AUDIO TRACK Play/Stop area is retired.
Existing automation names (`audiencePreview`, `previewOfflineNote`,
`clearControls`, `clearAllButton`, …) are preserved; the retired transport
mute name is `transportMuteAllCheck`.

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
| `LibraryBrowser` (`libraryBrowser`) | The sidebar's one browser: a FOLDER strip over the open folder's files, each in its own framed box and both exactly the width of the search field below, so the strip reads as a list and the current folder as a selected row rather than a dropdown. The strip lists Songs (catalog), Presentations (service shows + catalog records categorised as presentations) and Service (the running order) — never their files, so it stays three rows however full the Library gets. Choosing a row (`librarySongsHeader`/`libraryPresentationsHeader`/`libraryServiceHeader`) lists that folder in the pane below (`librarySongList`, `collectionLibraryList`, or the nested `currentServicePanel`); `libraryFoldersToggle` collapses the strip so the open folder gets its room. Every folder row is a drop target — Songs/Presentations reclassify a `catalog:` item, Service prepares a catalog song, a presentation, or a media cue at the end of the order, which is how a song reaches the service without opening it. PowerPoint files drop on Presentations (Library) or Service. Owns `catalog:<id>`/`show:<id>` drag sources, the song context menu (`librarySongMenu`) and rename dialog (`libraryRenameSongDialog`); `librarySearchField` sits at the BOTTOM of the browser under the files. Focused Delete confirms permanent Library removal. The redundant Add/Import/New/Remove buttons and manual-creation dialog are removed. Browsing never presents. |
| `CurrentServicePanel` (`currentServicePanel`) | The one running order (`collectionPlaylistList`, `collectionPlaylistRow`) with reorder/remove handles, `serviceRemoveSelectedButton`, scrolled insertion gaps, `catalog:`/`show:`/`entry:`/`media:` drop handling and bounded edge autoscroll. A `media:` drop creates a single-slide cue with that media as its background and no lyric layer. `showHeading` is false inside the Service folder, whose row already names the order; there it also draws flat inside the shared frame — no inner frame of its own, 28 px rows, the same edges as the folder strip and the search — so all three folders present the same list view. No service picker in this milestone. Clicking a cue selects it and browses its show; it never presents. |
| `ListSelection` | Shared stable-key selection model: ordinary click replaces, Ctrl toggles, Shift extends from the anchor, Ctrl+Shift adds a range, Ctrl+A selects all rows the title search keeps across BOTH library folders (including rows behind the other folder). Library keys are typed `catalog:`/`show:` identities; service keys are occurrence-specific entry IDs. Filtering prunes hidden selections; switching folders does not, because the folders are a view of one filtered list. |
| `SongImportPage` (`songImportPage`) | Off-air import page over `SongImport`: `songPasteButton` (normal clipboard only), `songTitleField`, `songCcliField`, `songAuthorsField`, `songCopyrightField`, `songSlideCollection`/`songSlideCard` (thumbnail target, keys `song:<plan>:<index>`), `songAddToLibraryButton`, `songCancelButton`, `songImportBackButton`. It owns thumbnails only (no Selection canvas), stands them down when hidden, and writes nothing before Add. |
| `SongPlanner` (session) | Not a visual part: the shared non-blocking measurement runner (`text.measure` cache, one batch in flight, latest-ask-wins) used by both the import page and the service bridge. Exposes no persistence. |
| `SongImport` (session) | Draft → measuring → ready/error → committed/cancelled; title and CCLI/author/copyright fields; `addToLibrary` is its only durable write. |
| `ServiceCatalogBridge` (session) | `addToService`/`refreshFromLibrary`/`refreshAllLinked`/`renameInService`; prepares a catalog song into one referenced service show and reacts to `CatalogController::itemChanged`/`itemRemoved`. Never edits the catalog and never presents. |

The Library search uses `PanelField.fixedPlaceholderText` for a stationary hint;
Ctrl+F focuses it without changing the service. It is the LAST row of the
sidebar browser, under whichever folder is open. The redundant toolbar Search
control is removed. More → Import song opens the same clipboard page as the
Library button, and completion/cancel returns to the originating page.
`PanelButton` leaves sufficient content height for labels on compact 24 px rows.
The open Media drawer leaves at least 400 px for the preparation area; the open
folder's list fills what the folder strip and the search leave, and the service
keeps its own scrolling viewport. Compact action widths include room for their
labels.

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
