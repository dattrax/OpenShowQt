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
| Selection | `Selection` | `WorkerController::requestSelectionPreview(sceneJson)` / `clearSelectionPreview()` | Off-air render of one scene. Readiness is `worker.selectionReady`; idempotent per payload with request generations. One page owns the lane at a time (the visible page); leaving calls `clearSelectionPreview()` |
| Thumbnail | `Thumbnail` + `key` | `WorkerController::requestThumbnails([{key, scene}], w, h)` / `clearThumbnails()` | Bounded batch, REPLACES the previous ask; cap 32 scenes; keyed arrival via `thumbnailReady(key)`. Keys must be stable and unique per producer (see each part) |

Every thumbnail ask is 16:9 with each dimension inside **32..512** — the
largest accepted frame at the 640×360 slot this app streams at — computed
from the tile's own picture width so a size a control picks can never be
refused on the wire (`ShowPage::bandPixels`, `ThemeEditorPage::viewThumbPixels`
are today's spellings of that rule).

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
  source,     // who answers when silence falls ("this theme", "the service",
              // "the Songs look", "the renderer default", "")
  editable,   // can this target store this field at all (capability)
  canReset    // is there an own value to remove (the Inherit button)
}
```

`target` is explicit identity, not “whatever is selected”:
`{kind: "theme"|"context"|"slide"|"service", id, view}` — theme identity,
view, slide and selected item stay distinct. Controls emit
`commit(field, value)` and `reset(field)`; the **host adapter** translates
them to the domain verbs, which remain separate, atomic and undoable:

| Target | Scalars (family/size/align/spacing/weight/slant/fit/min) | Effects | Box | Transition |
|---|---|---|---|---|
| `theme` (Songs) | `editTheme` for the base fields, `setThemeTextStyleField` for the style block | `setThemeTextStyleField("stroke"/"shadow", null)` whole-record clear | `setThemePlacement` / `clearThemePlacement` | `setThemeTransition` |
| `context` (Bible view) | whole-entry merge through `applyThemeContext` (carries untouched members incl. `transition`) | not storable until TV.3 — state the inherited source and disable | merged `box` (retires `inset` in the same edit) | `setTextContextTransition` |
| `slide` (Edit) | `customizeCurrentSlide` + `revertCurrentSlideField` | same verbs with the effect-record fields | primary-box geometry customizes `placement` | none (no per-slide override) |
| `service` | — | — | — | `setServiceDefaultTransition` |

A controls file must not walk theme chains or synthesize inherited
overrides to populate a field. Unchanged commits and refused edits cost no
undo; sliders/geometry gestures commit once on release and undo once; a
refusal re-reads the authoritative record.

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
| Consumers | AudienceMonitor (`OutputPanel`, Audience), `EditCanvas` (Selection + box chrome in `overlay`), ThemeEditor centre canvas (Selection + placement overlay), Show cue cards and Bible cards via `SlidePreviewCard` (Thumbnail), Bible selected canvas (Selection) |
| Automation | host-assigned `objectName` (existing ones preserved: `audiencePreview` stays the OutputPanel frame); the placeholder text node is named `<host>PreviewNote` when a page needs to assert on it |

### SlidePreviewCard

`src/ui/SlidePreviewCard.qml` (CS.2). Surface plus caption, selection/pending/
live chrome and activation/context signals. A card is a click target, not a
focusable button (cue stepping must keep the keyboard).

| | |
|---|---|
| Inputs | the embedded `SlidePreviewSurface` via the `surface` alias (bound through by the host: target, key, controller, text fallback as `surface.placeholder` with `placeholderVisible`); `caption` (string; the strip fills the room under the picture); `selected`, `live` (booleans — chrome only, the host computes them from acknowledged worker state; a pending go wears a host chip, not the card border); `idleColor`/`hoverColor` |
| Events | `activated()` (primary click), `contextRequested(mouseX, mouseY)` (right click, in CARD coordinates — the host maps it to wherever its menu lives) |
| Adapter | the host decides what `activated()` means: Show presents the cue; Bible selects the generated slide off air; Themes selects the view. The card itself never calls a presentation verb |
| Consumers | ShowPage cue tiles, Bible generated-slide cards, ThemeEditor Songs/Bible view tiles |
| Automation | `objectName` per host (`operatorCueTile` keeps its `index`/`modelData`-compatible properties for the cue tests); Accessible.name assembled by the host |

### SlidePreviewCollection

`src/ui/SlidePreviewCollection.qml` (introduced ungrouped in CS.3, grouped
in CS.7). Virtualized responsive preview collection with group boundaries,
index mapping and visible-band reporting. Implemented as a ListView over
computed visual rows so only visible rows have live delegates.

| | |
|---|---|
| Inputs | `model` (flat array of row maps with `cueKey`/`slideId`/… or generation slides); `grouped` (false = uniform flow — Bible; true = every item occurrence starts a new row — Show); `cellWidth` (operator zoom level); group boundaries come from the row's occurrence identity (`entryId`), never from show name or theme |
| Outputs | `cueRow(cueIndex)` / `cueRect(cueIndex)` — mapped geometry for scroll-to-cue; `visibleCueIndexes()` — actual visible cue indices (never headings/gaps/padding); column count; content height |
| Events | `cueActivated(cueIndex)`, `cueContextRequested(cueIndex, point)`, `visibleBandChanged()` (throttled — the host debounces its thumbnail ask off this) |
| Adapter | hosts keep their own flat presentation-order model (`cueRows` for Show; `scripture.slides` for Bible) and the collection maps visual cells onto it; the collection never reorders cues |
| Consumers | ShowPage (grouped), BibleImportPage (ungrouped) |
| Automation | `objectName` per host (`operatorSlideGrid` keeps its role for the cue suite); delegate instances keep `operatorCueTile` + `index` |

### AppearanceInspector

`src/ui/AppearanceInspector.qml` (CS.5). Scrollable composition of the
supported style sections for one adapter target; the host decides which
sections are visible.

| | |
|---|---|
| Inputs | `adapter` (InspectorAdapter), `target` (identity map above), `sections` (flags: typography, effects, background, geometry, transition, font) |
| Outputs | pass-through of each section's state |
| Events | `commit(field, value)` / `reset(field)` pass-through (host translates) |
| Consumers | ThemeEditorPage (Songs + Bible rails), EditPage (slide rail, CS.6) |

### TextStyleControls

`src/ui/TextStyleControls.qml` (CS.5). Family, size, alignment, line
spacing, weight, slant, fit and minimum size.

| | |
|---|---|
| Inputs | `adapter`, `target`; capability flags come from `fieldState(...).editable` — where today's Bible context cannot store a field (TV.3), the control states its inherited source and disables rather than writing the Songs base |
| Events | `commit(field, value)` (family/size/align/spacing use the base-field spelling, weight/slant/fit/minFontSize the style-block spelling), `reset(field)` |
| Consumers | AppearanceInspector; embeds `WorkerFontPicker` for the family row |

### TextEffectsControls

`src/ui/TextEffectsControls.qml` (CS.5). Outline/shadow whole-record
editing with enable/off/inherit state and resolved values.

| | |
|---|---|
| Inputs | `adapter`, `target`; shows the record this target owns when `isSet`, the resolved record otherwise (with the inherited badge) |
| Events | `commit(field, value)` with the TS.4 member spellings (`strokeEnabled`, `strokeColor`, `strokeWidth`, `shadowEnabled`, `shadowColor`, `shadowOpacity`, `shadowOffsetX/Y`, `shadowBlurSigma`), `reset("stroke")` / `reset("shadow")` for the whole-record clear |
| Rules | effects commit complete records; a member edit merges once through the controller verb; reset removes the whole override; sliders commit on release |

### BackgroundControls

`src/ui/BackgroundControls.qml` (CS.5). Colours/gradient plus supported
image/fit controls; exposes only operations the current target can store
and render.

| | |
|---|---|
| Inputs | `adapter`, `target`; `canPickImage` (slide only today) |
| Events | `commit("textColor"/"backgroundColor"/"backgroundGradient", …)`; `backgroundChosen(assetId)`, `backgroundFitChosen(name)` (slide verbs) |
| Note | keeps `""` gradient = explicit flat distinct from inherit |

### BoxGeometryControls

`src/ui/BoxGeometryControls.qml` (CS.5). Shared X/Y/W/H inputs plus a
follow/inherit action; the host binds it to theme placement, context box or
slide-customization geometry.

| | |
|---|---|
| Inputs | `adapter`, `target`, `rect` (live gesture value), `canReset` |
| Events | `applyBox({x, y, width, height})` (four fields commit as ONE unit), `resetBox()` |
| Rules | non-finite or <1 w/h snaps back; a drag and the typed path commit the same single undo |

### TransitionControls

`src/ui/TransitionControls.qml` (CS.5). Cut/Crossfade, both durations, and
optionally the Inherit entry and source sentence.

| | |
|---|---|
| Inputs | `shown` record (`{own, kind, textDurationMs, mediaDurationMs, seat}`), `allowInherit` (false on the service target) |
| Events | `kindChosen("inherit"\|"cut"\|"fade")`, `durationsCommitted(textMs, mediaMs)` |
| Rules | QML validates SHAPE only (digits); the 0..10000 band stays `Documents::validTransitionOptions`; a Cut disables the fields without erasing them; Inherit writes nothing and is enabled only against an existing record |

### WorkerFontPicker

`src/ui/WorkerFontPicker.qml` (CS.5). Search/browse of the worker-owned
family/face metadata, missing-font state and typed-name path.

| | |
|---|---|
| Inputs | `worker` (font-list properties), `currentFamily` |
| Events | `familyChosen(name)` |
| Rules | typed names remain usable with no worker; a missing-family warning requires a COMPLETED worker list; never Qt's installed fonts; the requested name is never rewritten by a fallback verdict |
| Consumers | `TextStyleControls` (popup + status line, extracted from `ThemeEditorPage.qml`'s font row) |

### BibleImportWorkspace (`BibleImportPage.qml`)

The existing page, restructured in CS.3: lookup/status first, a compact
scrollable verse-selection area (checkboxes stay the source selection with
readable verse text), a generated `SlidePreviewCollection` (ungrouped, worker
thumbnails keyed by plan generation + slide index), a substantial selected
`SlidePreviewSurface` (Selection target, in-page — the right Audience
monitor keeps showing the live output), then reachable Add/Cancel.

| | |
|---|---|
| Inputs (model) | `bible` (BibleImport session), `scripture` (ScriptureImport session) context properties, unchanged verbs |
| Owner state | `ScriptureImport` owns the selected generated-slide index and a stable transient preview identity: thumbnail keys are `scripture:<planGeneration>:<index>` (generation bumps on every publish) so a new plan can never paint over an old card; `previewSlide(index)` selects off air |
| Rules | browsing never presents; no network refetch or document/history edit for previews; hidden page asks nothing (Show/Themes/Bible cannot overwrite each other's thumbnail batches); obsolete selected pixels are cleared on selection/plan/page/connection change until the matching ask lands; Add keeps its one-undo/double-Add/notes/word-conservation contract (BI/TF evidence) |
| Wording | preview help says the picture is shown **on this page** (the earlier “preview panel” sentence was wrong — the right dock is the live Audience) |

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
| CS.6 | EditPage migrates to the shared inspector; advanced style fields via customization verbs; badges/Remove-customizations cover advanced-only changes |
| CS.7 | `SlidePreviewCollection` grouped mode in ShowPage (fresh row per running-order occurrence) |
| CS.8 | Thumbnail band + scroll-to-cue from actual grouped geometry |
| CS.9 | Integrated reuse, accessibility, capture and regression acceptance |
