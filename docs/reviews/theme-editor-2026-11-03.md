# Theme views, editor space and transitions: architecture review

Reviewed 2026-11-03 (Europe/London), against `a9b87a9` (TF.4).
Scope: inspect the delivered implementation and schedule changes. This review
does not implement the new editor, effects or transition controls. TH.1–TH.9
and TF.1–TF.4 remain completed implementation checkpoints; their tests do not
establish the new acceptance below. Work IDs are in [PLAN.md](../../PLAN.md).

The [implementation runbook in PLAN.md](../../PLAN.md#implementation-runbook--tvtstr)
now gives per-leaf file/API starting points, numbered steps, concrete field
defaults/limits, version and capability handling, async ownership and exact
test commands. It selects disjoint Bible caption regions for the first
implementation and defines an explicit opt-in layout recipe; the shared-region
alternative discussed below remains future design context. Use that runbook's
specific decisions when the review offers more than one possible approach.

## Recommendation

Use **theme views** in the interface: select a theme, then **Songs** or **Bible**
on the left, edit a large central canvas, and use a scrollable inspector on the
right. The existing named text contexts are a useful foundation, but are not
yet complete view definitions. Keep the existing appearance resolver and the
worker's text engine as the single implementation of inheritance and layout.

Give Themes the whole workspace below the application header. Temporarily hide
the library/running order, media bin and audience dock while editing themes;
retain compact audience state and the existing emergency clear shortcuts.
Returning to Show restores its panel dimensions and open/closed state.

Songs/Bible describe **content**. Lower thirds describe a **layout for an output**;
NDI is the delivery mechanism to OBS. Stage is another output with different
content needs. Reserve a separate output-profile dimension so a future song can
appear on the projector, as a lower third and on stage simultaneously. A single
`textContext = "ndi"` cannot express that requirement.

## Reference assessment

The supplied [editor screenshot](https://support.renewedvision.com/hc/article_attachments/56372370946579)
shows one selected theme, a left list of Title/Main Point/Scripture/Quote/Lower
Thirds layouts, a large canvas and a separate object list/inspector. Those are
visual layout variants, rather than evidence of separate output transports.
The [theme guide](https://support.renewedvision.com/hc/en-us/articles/34551484745875-Guide-to-Using-Themes-in-ProPresenter)
also describes typography, automatic sizing and text effects. These references
inform the proposed navigation and styling scope; arbitrary shapes, scrolling
text, text linking and ProPresenter media actions are not added to this milestone.

## Delivered architecture and findings

Paths/line numbers below identify the reviewed revision, not a promise that
later implementation leaves those lines unchanged.

| Finding | Evidence and consequence | Follow-up |
|---|---|---|
| **R1 — The editor is a mode, but uses only the operator centre column.** | `src/ui/Main.qml:143` keeps the two docks and media bin present on Themes; the page anchors between them and above the bin. `ThemeEditorPage.qml:495` reserves 230 px for themes and `:788` reserves 250 px for the inspector, plus gaps/margins. At the captured 1680×1020 size, the actual canvas is about 540×304. With 240/320 px outer docks at 1000 px, the same fixed inner columns leave a negative centre width. | TV.1, TV.2 |
| **R2 — Inspector controls can exist without being reachable.** | `ThemeEditorPage.qml:788` is a plain `Column`, with no scroll viewport. Typography/placement consume its available height and Scripture Reading follows below it. In the current 1680×1020 capture the Scripture section is occluded below the editor/media boundary. The list's action row can also exceed its 230 px column. `workspace_ui_tests` tests values and invokes handlers; the theme capture is only at 1680, and no assertion proves every control is visible/reachable. | TV.2, TV.5 |
| **R3 — Bible styling already exists, but has no selectable view.** | `documents.h:437` defines sparse `TextContext`; `Slide.textContext` is empty for songs and `scripture` for imports. Contexts override font family/size, alignment, line spacing and box/inset. They explicitly cannot carry colours/effects; format 7 rejects unknown entry keys. The inspector hard-codes one Scripture Reading section instead of selecting a context/layout. | TV.1, TV.3 |
| **R4 — The main sample never previews Scripture Reading.** | `DocumentController.cpp:1008` clones base theme fields and placement into a throwaway theme, but copies neither `contexts` nor a slide context name. `sampleAsk` always calls this method without a view. The draggable overlay uses base placement too. Editing Bible typography/box therefore cannot be judged on that canvas. The approximate-lines probe at `ThemeEditorPage.qml:398` also omits `fontFamily` and lacks a full style generation tag, so it is insufficient evidence for Bible preview parity. | TV.1, TV.5 |
| **R5 — Context precedence needs to be preserved deliberately.** | `Documents::appearanceForSlide` (`documents.cpp:1086`) first chooses the **first matching context as a whole** from current → slide → show → Default. Individual missing fields then fall through to the **base theme chain**, not to matching contexts on lower themes. Slide customizations win above it. Thus a lower theme's Bible context can outrank a higher theme's base size if the higher theme has no Bible context. This is tested in `domain_tests`, but some prose suggests merging all matching contexts. | TV.3 |
| **R6 — Effects and font faces are missing throughout the authoring path.** | `Theme`, `SlideCustomization`, `ResolvedAppearance` and `SceneItem` carry no font weight/slant, stroke or shadow. `SceneText.cpp:305` sets family, size and colour only. `GroupAlphaPainter` already implements shadow drawing during fades, and the pinned public SkParagraph headers expose font style, shadows and foreground paints. These are renderer building blocks, not completed effect support. QML-only effects would disagree with audience output. | TS.1–TS.5 |
| **R7 — Text shrinking is already implemented, with a fixed policy.** | `SceneText.cpp:323` wraps and shrinks each text item in up to three passes with a minimum size. `text.measure` measures unshrunk text. There is no authorable fit/minimum-size policy and no effect-ink bounds in the measurement result. Stroke/shadow can extend outside a box even when paragraph height fits. | TS.2, TS.3 |
| **R8 — Bible caption space is reserved, but captions are not slide objects.** | `scripturepagination.cpp:59` measures reference/attribution and subtracts their height. `ScriptureImport::publish` retains only body lines and a verse label; Add stores that label in notes and provenance on the show. `compileScriptureFixture` compiles only the passed body lines. The reference/translation/copyright have no separately styled visible slots. All existing text items also share slide typography (`scene.cpp:470`); adding another box alone cannot provide independent caption styling. | TV.4a, TV.4b |
| **R9 — Pagination and later restyling are different operations.** | The Bible page re-paginates transient fetched words when documents change **while that page is visible** (`BibleImportPage.qml:20`). There is no show reflow operation after Add: saved slides are ordinary authored items, and a later theme change restyles/shrinks them without rebuilding slide boundaries. While the page is hidden, theme edits need a fresh style check on re-entry/before Add; otherwise a previously measured batch can be stale. | TV.5 |
| **R10 — Crossfade is not a user setting.** | `SceneRenderer.cpp:327` fixes text at 0.5 s and media at 0.8 s. `RpcMethods.cpp:521` accepts `fade` or `cut`, defaulting to fade. `PresentationModel.cpp:52` sends only the prepared revision, so normal UI presentation always takes that default. Neither the service nor `AppSettings` stores a transition policy. | TR.1–TR.3 |
| **R11 — Transition timing must travel with the presentation intent.** | Prepare/present are asynchronous; `PresentationModel` owns their correlation, `SceneStore` owns desired/landed/published commands, and `SceneRenderer::captureFrame` fixes the two blends once for both audience and mirror preview. Reading mutable settings on prepare acknowledgement would let an older Go pick up a newer setting. Restart, clears, restore, rapid Go and failed presents all use these same seams. | TR.1–TR.3 |
| **R12 — NDI/stage require output architecture work.** | There is one audience session/compositor, one acknowledged live scene, a 1920×1080 authored canvas and RGB colours without authored background opacity. Preview targets are consumers, not independently themed outputs. Transparent broadcast composition, sender pacing, OBS reception and stage current/next/notes bindings are absent. A theme row or new context name cannot supply them. | FO.1–FO.6 (later) |

Useful foundations to retain: Qt-free domain/history; validated versioned storage;
immutable resolved scene payloads; measured generic `textflow`; asynchronous
off-air preview with request ownership; bounded prepare/cache machinery; one
audience frame shared with its mirror; and existing no-extra-target/public-Skia
API guards. A replacement document owner, QML text renderer or second paginator
would duplicate working responsibilities.

## Proposed view contract

Implement incrementally rather than rewriting the completed text-context model.

| UI selection | First implementation | Inheritance / persistence |
|---|---|---|
| Songs | Existing base theme appearance and primary placement | Existing empty `textContext` remains the song/default spelling. No duplicate song record is required. Clearly label these values as the theme defaults also inherited by Bible. |
| Bible | Existing named `scripture` entry, promoted to a richer view override | An absent view uses the theme defaults. A present view overrides only fields it owns. Existing typography and box/inset resolve exactly as format 7 does. |
| Other existing context names | Preserve their names and values; expose as additional views where supported | Never discard an unfamiliar name on edit, duplicate or save. Unknown **keys** still fail validation. |
| Lower thirds / Stage | Clearly marked planned destinations, with a short explanation or hidden until implemented | Do not let these select a live sender or overwrite a slide's content context. |

TV.1 first makes Songs/Bible navigation and preview work using **format 7
unchanged**. TV.3 then extends the view's sparse properties to text/background
colour, gradient and the styling fields introduced by TS.1. Retain `contexts`
and `textContext` as wire/storage identifiers for this increment; the UI says
"view". Explicitly supersede TF's colours-prohibited rule only for the new
service format. Do not teach the format-7 reader a new meaning.

Resolution stays:

1. Slide customizations, per field.
2. The first matching named view in current → slide → show → embedded Default.
3. Base theme values in that same theme order, then renderer defaults.

The selected view's absent fields fall directly to step 3. Do not merge lower
namesakes. For Songs, step 2 is absent. Add provenance to the resolved editor
model (customized / view / theme / default) so the inspector can show inheritance
and clear one override. An explicit disabled effect must override an inherited
enabled effect; absence means inherit. Store each stroke/shadow as an optional
validated record with `enabled`, rather than using zero/empty as both meanings.
Removing a Bible override never clears the parent Songs/default value.

TV.3 introduces the next unclaimed service format (TS.1 may already have claimed
8 for base styling): read formats 1–7 and any intervening format, preserve IDs, current theme, customizations,
unknown context names, authored geometry and embedded Default. Migrate old
fields without inventing overrides; old documents must produce identical
pixels before an operator makes a new edit. New keys in old-format files and
mixed/invalid view records fail atomically. Later styling/role/transition leaves
must version any further persisted meaning they add; they must not redefine an
already shipped format. Scene schema is currently **3**, protocol **0.14**;
negotiate additions and refuse unsupported effects instead of silently dropping
them. The worker continues to receive fully resolved scene items, not themes.

### Workspace and preview

```text
Application header: Theme Editor · audience state · return to Show
┌──────────────────┬─────────────────────────────────┬───────────────────┐
│ Theme picker     │ Selected theme / view           │ Scroll inspector  │
│ New / duplicate  │                                 │ Text / geometry   │
│ Songs thumbnail  │ Worker-rendered canvas          │ Stroke / shadow   │
│ Bible thumbnail  │ + selection/resize handles      │ Background        │
│                  │                                 │ Transition        │
│                  │ Sample selector / preview state │ Inherit / revert  │
└──────────────────┴─────────────────────────────────┴───────────────────┘
```

Use bounded/resizable side columns (roughly 200–240 and 280–320 px), fit the
16:9 canvas into the remaining area, and scroll/collapse inspector sections.
At the existing 1000×680 minimum, both columns and a positive canvas must fit.
Use layout constraints rather than another collection of fixed widths/heights.
Changing mode must not write persisted Show panel sizes. Clear drag/focus/preview
ownership on exit; keyboard presentation must not fire while typing.

The preview fixture must copy the **whole** selected theme (including a modified
embedded Default), select the requested view and compile through the production
resolver. Offer representative song and synthetic scripture samples (long text,
Unicode, numbers, multiline reference and attribution). A view switch updates
the sample, overlay and inspector together; dragging edits the selected view's
box. Switching theme/view/worker generation retires old image and measurement
replies. Never carry a song image beneath Bible selection handles. Show a useful
disconnected state and preserve editable values when the worker is unavailable.

Theme browsing and off-air preview must not send `scene.present`. Retain the
existing explicit "Use as current theme" operation as a separate action; it is
a real undoable document edit, and must keep the established live-edit policy.

### Bible roles and stale flow

TV.4a/b add a small, explicit layout contract for **body**, **reference** and
**attribution** roles; reference can include translation. This is not an
arbitrary template engine. Give each role its own sparse typography and box,
inheriting the view's text appearance. Bind generated content to those roles in
authored items, resolve in the frontend and emit ordinary flat `SceneItem`s.
Keep the first-text primary-box rule for legacy untagged items; never infer a
caption role from stack position or accidentally move every caption with the
body. Respect the existing eight-item cap.

Measurement, preview, Add and playback must use the same resolved role geometry
and styles. For disjoint caption boxes, validate the body region and fit captions
in their own regions; for a shared region, reserve the actual caption ink/gap
and position the body in the remaining region. Do not reserve unrelated space
while vertically centring all words in the original full box. Explicitly test
long attribution, empty optional reference, multiline reference and an impossible
layout. Do not retroactively draw new captions on old services: migrate old items
unchanged, and offer explicit regeneration/adoption of the new Bible layout.

Before Add, require a plan generation keyed by selected verses, resolved style,
role geometry, worker connection and target schedule. Re-entering the Bible page
after theme edits triggers regeneration from its in-memory selection, without
another lookup. Disable Add until the current generation is measured and
previewable. Save generated scripture role items and provenance in the existing
one undoable schedule operation; playback remains independent of the provider.
Already committed passages keep their authored words and slide boundaries on a
theme edit. Automatic post-Add reflow needs retained source blocks and a separate
explicit undoable operation; it is deferred, and the UI must not claim otherwise.

## Typography and effects contract

TS.1 defines font family plus numeric weight and normal/italic style, horizontal
alignment/spacing, fit policy/minimum size, fill colour, one optional stroke
(enabled, colour, width) and one optional shadow (enabled, colour, opacity, x/y
offset, blur). Coordinates/width/blur are authored canvas pixels, scaled once
with the canvas. Reject non-finite, negative and out-of-budget values in domain,
storage, scene codec and RPC. Define conservative numeric limits before enabling
controls. Disabled effects and legacy font/fit defaults must preserve old pixels.
Variable-font axis editors, multiple shadows and rich text are later work.

Use SkParagraph/Skia public APIs and the shaped glyph runs. Stroke draws behind
fill without a different wrap; shadow uses the same glyph geometry. Apply the
current frame's transition alpha to fill, stroke and shadow consistently. Keep
effect preparation/caches bounded and out of presentation callbacks; retain
thread-owned paragraph painting and the no-`saveLayer`/no-extra-render-target
composition rule. Reuse the existing shadow painter support where appropriate.

TS.3 adds a measured **ink bounds** result alongside paragraph height. Include
stroke expansion and shadow offset/blur in safe-area/fit decisions; allow
negative offsets. Update importer reservation, cache keys and font-style
measurement together, including the Theme Editor's approximate-lines probe.
Fit modes should be explicit: preserve the current shrink policy for old slides;
Bible generation measures at the selected size and reports impossible layouts,
rather than silently changing size to make its pagination appear correct.

The font field should become a searchable installed-family/face picker with
sample text; distinguish the requested family from an unavailable/fallback
family. Inspect fonts through the worker's font configuration so Qt and Skia do
not silently offer different choices. Font downloading/installation is not a
normal editor action. TS.4 carries controls and TS.5 verifies pixels with a
pinned font corpus and a deliberate missing-family case.

## Transition contract

Keep transitions as **presentation options**, separate from a scene's static
appearance. Add a service default and optional theme-view override; an unset
view inherits the service value. Resolve the incoming cue's options when Go is
issued, capture them in that intent, then carry them through prepare ack →
`scene.present` → `SceneStore` → landed renderer transition. The selected theme
sample cannot change the next Go's options. No per-slide transition override is
needed for the first increment.

The UI exposes **Cut / Crossfade**, **Words/items duration** and **Background
duration**, in milliseconds (initial proposed range 0–10,000). Keep existing
defaults 500/800 ms. Duration 0 means immediate on that track; Cut ignores both.
The foreground duration covers text **and foreground image items**, matching
the current draw stack; the background duration covers colour/still composition.
Do not promise dual-video dissolves: the existing renderer bypasses a background
crossfade when video is involved. State/test that limitation in TR.2 and retain
continuous video playback when the background identity is unchanged.

Editing settings affects subsequent Go intents, not a fade already running.
First scene, emergency clear, restore and rapid re-trigger behaviour must be
specified and tested explicitly: start first scene immediately; emergency clears
cut immediately; restore uses the options captured by its explicit restore
intent; a rapid Go retains the current renderer's incoming-scene replacement
rule (it may restart from the last target, rather than snapshotting a blend).
Do not allocate an intermediate target to conceal that existing limitation.

Zero-duration tracks complete at their first captured frame (the current
`elapsed <= 0` branch would otherwise return zero). A transition retires only on
a successfully presented settled frame. Audience and mirror still draw the same
`SceneFrame`; off-air previews never advance its clock. Support old commands
with 500/800 defaults and negotiate any new duration fields. Invalid policies
change no desired/published scene; UI live markers still await acknowledged
output, and a restarted worker still starts black.

## Work order and scheduling estimate

Estimates are focused engineer-days including the listed tests, assuming the
current build environment; they exclude productization/physical acceptance.
Every leaf is 0.5–2 days. Re-estimate after the model/renderer prototypes.

| Track | Leaf estimates | Dependencies and completion evidence |
|---|---|---|
| View/editor | TV.1 1d; TV.2 1d; TV.3 2d; TV.4a 1.5d; TV.4b 1.5d; TV.5 1.5d | TV.1 → TV.2; TV.3 follows TV.1 and TS.1's shared schema contract; TV.4a → TV.4b follows TV.3; TV.5 follows all TV leaves and TS.3 for final styled flow. |
| Styling | TS.1 1.5d; TS.2 2d; TS.3 2d; TS.4 1.5d; TS.5 1d | TS.1 → TS.2 → TS.3; TS.4 follows TV.2/TV.3 and TS.2/TS.3; TS.5 closes both rendering and control acceptance. |
| Transitions | TR.1 1.5d; TR.2 1.5d; TR.3 1d | TR.1 → TR.2 → TR.3; TR.1's persisted theme-view override follows TV.3. Service-only prototype can precede that. |
| Integrated gate | TA.1 1d | All TV/TS/TR leaves; final capture/test evidence and updated architecture/handoff. |

Suggested single-engineer order: TV.1 → TV.2 → TS.1 → TV.3 → TS.2 → TS.3 →
TV.4a → TV.4b → TS.4 → TV.5 → TS.5 → TR.1 → TR.2 → TR.3 → TA.1.
Total initial estimate: **21.5 engineer-days**, before M7. This is a scheduling
estimate, not a deadline or evidence of completion.

Later output work is gated separately: FO.1 output-profile contract (1–2d),
FO.2 alpha-aware lower-third composition (2d), FO.3 sender/dependency/OBS
compatibility spike (1–2d), FO.4 paced NDI sender (2d), FO.5 stage bindings and
independent composition (2d), FO.6 output acceptance (2d). FO.4/FO.5 must be
split further if their prototypes exceed two days. Do not schedule these
against this estimate or claim sender/stage readiness from TV completion.

## Required test evidence

Add tests with each implementation leaf, not as a final standalone test phase.
Use production seams; add fixtures for behavioural failures rather than tests
that mirror QML source text.

| Area | Required assertions | Existing entry points to extend |
|---|---|---|
| Views/inheritance | Songs unchanged when Bible differs; absent/matching/unknown view; first-context wholesale selection; sparse field fallback; customization/revert; explicit disabled effect; protected Default; duplicate/delete; one undo per gesture; old-format pixel identity; malformed records fail without mutation. | `domain_tests`, `scene_tests`, `document_controller_tests` |
| Preview ownership | Songs↔Bible updates pixels and handles; full copied theme including edited Default; context box drag affects Bible only; rapid switching/late measure/worker restart never shows mismatched imagery; browsing does not present; current-theme action remains explicit. | `workspace_ui_tests`, `preview_selection_tests`, `worker_controller_tests`, `frontend_journey_tests` |
| Workspace reachability | Themes uses space below header; every view/action/inspector field reachable by mouse and keyboard; no overlapping/negative bounds; scroll to final effects control; return to Show restores panel sizes/drawer/focus. Capture Themes at **1000×680, 1380×800, 1680×1020**, each Songs/Bible, plus disconnected and long-name states; include 150/200% scaling. | `workspace_ui_tests`, `workspace_panels_tests`, `ui_loads`; extend capture harness beyond the single existing Themes capture |
| Styled pixels | Stroke edge/fill order; shadow positive/negative offsets and blur; disabled/zero effects identical to legacy; font face changes; fallback/Unicode; safe-area bounds at different scales; midpoint fade affects every effect; render/cache budgets remain bounded. CPU and GPU comparisons use tolerances, not cross-driver byte-equal goldens. | `scene_render_tests`, `unicode_text_tests`, `scene_frame_tests`, `live_fade_tests`, public-API/render-target guards |
| Bible flow/roles | Measurement and rendered ink fit same body/caption regions; long attribution and reference, impossible layouts; selected words conserved/in order; theme edited while Bible hidden forces fresh generation before Add; superseded measurements cannot enable Add; target changes invalidate Add; one undo, save/reopen, offline playback; old saved passages keep boundaries. | `textflow_tests`, `scripture_pagination_tests`, `text_measure_tests`, `bible_import_page_tests`, `bible_import_e2e_tests` |
| Transition policy | Defaults/custom persisted options, view inheritance, Cut, zero/unequal durations, NaN/negative/over-limit/unsupported commands; options belong to the original Go across delayed prepare and settings changes; rapid Go/cut interrupts, clear/restore, black restart; same-background video continues; video-switch limitation explicit; idle returns only after landed fade completion. | `protocol_tests`, `worker_controller_tests`, `scene_store_tests`, `scene_frame_tests`, `render_scene_transition`, `live_fade_tests`, `document_present_tests`, `production_journey_tests` |
| Future destinations | Same cue produces different audience/broadcast/stage layouts concurrently; alpha over checkerboard then real OBS reception; profile changes do not repaginate audience slides; sender disconnect/backpressure does not stall audience/audio; stage next/notes never leak to audience. | New output-profile/sender fixtures under FO; physical OBS/network and second-display checks recorded separately |

### Fonts and local inspection artifacts

Downloaded three contrasting families from the official
[Google Fonts source repository](https://github.com/google/fonts), pinned to
`2fe1e8c6b137fb13041a7cfca6fdcd8a910ea4dc`, with each family's `OFL.txt`.
Files are local stimulus under `/tmp/openshow-theme-review/fonts/`, not installed
system fonts or committed product assets. A private Fontconfig configuration
lets the production CPU snapshot renderer use them without changing the machine.

| Font source below `ofl/` | Purpose | TTF SHA-256 |
|---|---|---|
| `inter/Inter[opsz,wght].ttf` | Sans, song typography / numeric weight cases | `29160a80ff49ddcab2c97711247e08b1fab27a484a329ce8b813d820dc559031` |
| `lora/Lora[wght].ttf` | Serif, reading and different wrap/metrics | `822a6621ccbe8d97d20ac88c1c41f5615c9c2c202eaa75f272cd452aac6475a7` |
| `oswald/Oswald[wght].ttf` | Condensed, lower-third stimulus | `5b38c246e255a12f5712d640d56bcced0472466fc68983d2d0410ec0457c2817` |

TS.5 should turn this into a small reproducible fixture corpus: pin source
revision/checksums/licences, use an isolated Fontconfig/cache, include a fixed
fallback face, and run without network. A missing required font must fail or
report a visible skip, rather than quietly testing a different face. Arbitrary
variable-axis controls are not implied by using variable font files.

Current inspection artifacts (temporary; current UI captures are reproducible
using the existing harness):

- `/tmp/openshow-theme-review/propresenter-themes.png`: supplied reference.
- `/tmp/openshow-theme-review/current.png.themes.png`: current 1680×1020 Themes
  capture; disconnected preview intentionally blank, layout still measurable.
- `/tmp/openshow-theme-review/font-stimulus.png`: 1440×810 production CPU-rendered
  comparison of the three downloaded families using synthetic review text.

## Verification performed for this review

- `cmake --build --preset desktop --parallel 2`: passed.
- Focused baseline command below: 13/14 passed in the restricted sandbox;
  `worker_controller_tests` could not handshake with any of its local scripted
  peers. Rerunning it with local socket access passed. The second command's
  **9/9 passed**, including six live-display fixtures, with no skips. Across
  both commands, **22 distinct checks passed**. This is the relevant current
  baseline, not a fresh full-preset or physical-projector acceptance claim.
- `workspace_ui_tests` rerun with offscreen/software Qt and
  `OPENSHOW_WORKSPACE_CAPTURE=/tmp/openshow-theme-review/current.png`: passed;
  inspected its `.themes.png` capture and the supplied reference image.
- `fc-scan` recognized all three downloaded font families. With the private
  `FONTCONFIG_FILE`, `openshow-worker-dev --snapshot` rendered the comparison
  from a scene-3 fixture; inspected the resulting PNG. No fonts were installed.
- Document checks passed: local Markdown targets across the five changed
  documents, 22 unique follow-up IDs, the 21.5-day estimate sum and
  `git diff --check`.
- Runbook follow-up: checked all local anchors, 39 source file entry points,
  15 implementation leaf sections and four focused selectors against registered
  ctest names. Fixed the existing M6 closure link's outdated anchor. This
  follow-up changes documentation only; it did not rerun application tests.

```sh
ctest --test-dir build -R '^(domain_tests|scene_tests|protocol_tests|textflow_tests|scripture_pagination_tests|scene_frame_tests|scene_store_tests|scene_render_tests|unicode_text_tests|workspace_ui_tests|document_controller_tests|worker_controller_tests|workspace_panels_tests|render_scene_transition)$' --output-on-failure -j1

ctest --test-dir build -R '^(worker_controller_tests|document_present_tests|text_measure_tests|bible_import_page_tests|bible_import_e2e_tests|live_fade_tests|preview_selection_tests|render_layer_guard|skia_public_api_guard)$' --output-on-failure -j1
```

New acceptance tests in this document are scheduled work. No product code or
test sources were changed by this review.
