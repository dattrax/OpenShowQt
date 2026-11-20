# OpenShowQt progress and handoff

Updated: 2026-11-19 (Europe/London).

This file is the CURRENT handoff. Earlier checkpoint narratives — the media
library, the type/effects review, the CS/TV/TS/TR series and the M6 milestones —
are archived verbatim in
[`docs/history/progress-archive-2026-11-16.md`](docs/history/progress-archive-2026-11-16.md);
git history is the authority for what each of those commits did. Nothing in the
archive is a current instruction.

## Current checkpoint

**Persistent service-card thumbnails (TC.1, 2026-11-20): contract frozen.**
`docs/ui/thumbnail-cache-plan.md` fixes the optional, independently versioned
`thumbnailCache` block in `service.json` (service format stays 15), the entry
bounds (256 records / 32 KiB PNG / 1.5 MiB encoded), the single 512x288 bucket,
the fingerprint inputs (normalized scene JSON, asset size + 8 KiB content probe,
renderer identity), the Qt-free DTO/codec API and the overlay read/write rules.
The bucket was chosen from measurements of all 162 repository card renders
(512x288 average 3 809 B PNG / 5 080 B Base64, 0.27 ms decode; 256x144 1 723 B
/ 2 298 B). TC.2–TC.10 remain TODO; no runtime evidence is claimed yet.

**Edit follow-up (EW.11, 2026-11-19): corrected.** Upward/no-op drops used
`dragFrom = -1` after drag cleanup and subtracted a slot incorrectly; compute
the destination before clearing state and validate the release in both axes.
Up/Down now use worker hit testing and a remembered horizontal position,
including wrapped/empty lines, short lines, Shift selection and rapid keys.
Clicking blank canvas outside the edited box commits/exits directly; inspector
focus is preserved and the Done button is removed. UI rebuilt; workspace and
UI-load checks passed (65.03/8.02 s), and the expanded real-worker editing
journey passed at default scale and `QT_SCALE_FACTOR=1.5` (15.37/15.40 s).
`git diff --check` passed.

**WYSIWYG editing, slide ordering and insertion (2026-11-19): EW.1–EW.10 are
DONE.** The Edit stage now edits the words on the slide, the rail reorders and
inserts slides by pointer or keyboard, and both paths are one document history
entry each. The frozen contract is
[`docs/ui/wysiwyg-editing-plan.md`](docs/ui/wysiwyg-editing-plan.md); the
architecture section is "WYSIWYG editing, slide ordering and insertion
(milestone EW)".

Protocol minor **21** adds `text.editor.v1` / `text.editor`: a compiled scene,
one text item, an optional bounded draft and caret/selection/point asks
answered from the renderer's own `layoutInBox`. Carets come from glyph cluster
bounds, selection from the paragraph's range rects, hit tests from the
paragraph, and authored UTF-16 offsets map through the same uppercase transform
that renders (so `ß` → `SS` still names the authored character). Nothing is
stored, prepared, presented or cached.

`InlineTextEditor.qml` is the native input bridge: a transparent focused
`TextInput` for IME/clipboard/navigation/line breaks, worker-derived caret,
selection and composition chrome, and pointer selection by worker hit tests.
Escape owns the session first (then the rail grab, then selection/output), and
click-outside, focus exit, navigation, undo/redo, save and page leave commit or retire
the session deterministically. The persistent bottom field became an explicit
**Source text** drawer, opened automatically and labelled when the worker
cannot answer geometry.

`DocumentController` gained `moveSlideFor`/`moveCurrentSlideTo`/
`moveCurrentSlideStep` and `insertSlideFor`/`insertSlideAt`/
`insertSlideAfterCurrent`: explicit show/slide/anchor ids, final-slot
destinations, selection by id across moves and undo/redo, one history entry,
inherited section/context/background/fit, no copied words/notes/items/style,
and an untouched leading cue. The rail drags a visible grab handle with a ghost
and insertion gap, bounded edge autoscroll, Escape/outside/no-op/model-change
cancellation, row and keyboard moves, and Insert before/after; the footer is
**Insert slide**.

Verification: the full desktop suite passed **every runnable entry with `-j4` (88/88)** (the 89th
entry, `font_fixture_tests`, is **skipped**: its pinned-corpus cache was
removed during this work and the sandbox has no network to fetch the pinned
fonts again; it is not an EW test and its earlier cache-based failure was in
stroke/shadow ink bounds untouched by EW). Parallel runs saw only the known
timing-sensitive display flakes: `workspace_ui_tests` at the pre-existing
camera-Delete check (`-j8`, once) and `frontend_journey_tests` at the cue
live-marker check (`-j8` once and `-j4` once, after a clean `-j4` run). Each
passed alone, in a 3-way concurrent run, and in the clean `-j4` run. The headless suite passed **48/48** (`-LE display`). The real-worker
editor journey and the whole-shell Edit journey pass; the editor-layout round
trip measured median **0.02–0.03 ms**, p95 **0.02–0.04 ms** over 40 asks on
this machine, well inside the 150 ms budget. Editing, moving and inserting
never presented; the workspace suite and the real-worker Library/Edit journey
also pass at `QT_SCALE_FACTOR=1.5`. Deleting the edited box retires the
session, and insert→type→reorder→undo/redo is exercised on the real shell.
IME composition is implemented as the native preedit plus decoration, but no
physical input method was exercised here. Physical gates (projector second
display, NDI, target GPU) remain open under M7/ND/TA and are not claimed.

**Reflow section reconstruction (2026-11-19): corrected.** Reflow previously
treated each stored slide fragment as a new stanza, retaining old pagination;
its saved source also became stale after the first reflow. It now joins
consecutive fragments of each named verse/chorus before measuring and splitting
again. Generated `(cont.)` suffixes are excluded from section identity, and
repeated choruses separated by other sections retain their order and words.
New song preparation captures group identity before continuation notes. A
successful reflow updates the fingerprint of an already-valid source in the
same service edit, so later fitting changes can restore authored lyric lines;
edited/stale sources remain excluded. Legacy/current-word fallback also joins
its consecutive section fragments. Regressions cover larger/smaller boxes,
repeated fitting changes, original lines/words, repeated choruses, save/reopen
and the leading cue. UI rebuilt; reflow, document controller, theme apply,
UI-load and real-worker planner checks passed **5/5**.

**Import/reflow text fitting (2026-11-19): corrected.** Measurement used line
metrics alone and deliberately ignored effect spill; tight line spacing could
therefore approve visible glyph overhang, while outlines and shadows crossed
the box. `SceneTextEngine::layoutInBox` now shares wrapping/insets between
measurement and painting, including shaped glyph bounds and space for enabled
effects. It preserves the chosen size when paginating and can produce more
slides. Import/reflow send the real box height through `WorkerController`,
including an explicit centre anchor, and height participates in measurement
cache identity. Bible reflow now resolves the chosen snapshot's target context
(typography, box or inset) rather than measuring its Songs view.

Regression coverage checks rendered pixels at all nine alignment combinations,
tight-leading glyph overhang, a styled clipboard import, height forwarding and
cache identity, and a passage whose Bible box is smaller than its Songs box.
The latter remeasures the committed slides and verifies every original-case
word is retained. Preview/theme selection is unchanged, as requested. Restart
the rebuilt application and use Reflow text on existing imported items to
regenerate their breaks with the corrected measurements.

UI/worker and affected test builds passed. Focused ctest checks passed **15/15**
(67.26 s), including rendered-pixel containment, real-worker song/Bible import
and reflow, worker adapter/protocol, pagination, domain and UI load. The final
Default-context inheritance correction passed document-controller, UI-load and
real-worker reflow checks again (4.18/8.02/1.17 s). `git diff --check` passed.
These are targeted checks; no new full-suite or physical-projector claim is made.

**Dialog wording (2026-11-17): simplified.** Apply theme now offers **Keep slide
breaks**, **Reflow text** and Cancel. Remove the normal explanation paragraphs
and single-occurrence count; retain a short Library/future-use note, repeated-
entry scope only when needed, and progress/refusal messages. Renderer refusals
use the same slide-break wording. UI rebuilt; existing workspace, UI-load and
real-worker reflow checks passed **3/3** (63.17/8.02/1.04 s).

**Apply-theme corrections (PT.9, 2026-11-17): fixed and verified.** The dialog
was bounded by the narrow sidebar while its children overflowed. Snapshot reuse
compared sparse themes with resolved values, creating same-named copies; the
reported service then reapplied an older copy with All caps off. The dialog now
centres in the window and fits its actions. Resolved-look matching reuses the
chosen local theme; different looks get unique labels, including old duplicates
on load/autosave recovery, without changing IDs or assignments. Rename refuses
collisions. Explicit creation also refuses an existing trimmed name without
mutation; generated New/Duplicate/import operations suggest a free name before
creation. Post-measurement transaction errors remain visible.

Name-prohibition follow-up: duplicate trimmed creation and Rename are refused
without mutation; repeated Duplicate chooses a free label. The rebuilt UI and
six focused checks passed again (document controller 4.20 s, workspace 63.08 s,
UI load 8.03 s, real-worker reflow 1.04 s, domain and apply transaction).

Build of the UI and affected tests passed. Focused ctest checks passed **6/6**:
domain, document controller, apply transaction, workspace (62.42 s), UI load and
real-worker reflow (1.03 s with local display/socket access; the sandboxed worker
run could not open its display). The workspace also passed at scale 1.5, with
dialog bounds/actions checked at 1000/1380/1680 widths. Reflow verifies updated
All caps, preserved source words, no copied theme, compiled preview and
save/reopen. `git diff --check` passed. Restart the rebuilt application and
explicitly apply the original Brun to replace the older remembered look; its
copy will have a distinct label. Audience still changes on Go. These are focused
checks; earlier full-suite totals below remain historical evidence.

**Whole-item themes and reflow (2026-11-17): PT.1–PT.8 are DONE.**
The group is closed; docs are reconciled and the desktop/headless suites plus
the scale-1.5 workspace run pass. The
[whole-item theme/reflow brief](docs/ui/presentation-theme-reflow-plan.md) moves
theme application to left-hand song/passage rows and offers keep breaks or
measured reflow. Library songs remember the chosen look by value for future
services; applying replaces text/layout overrides and preserves words/media.
The brief covers portable snapshots, explicit item precedence, offline/current-
word reflow sources, eligibility, stale work and separate Library/service history.
Completed groups below retain their evidence.

PT.1 added `ThemeSnapshot` and its validated codec. The value is the domain's
own appearance records — colour, type, both alignments, All caps, spacing,
weight/slant/underline, fit/minimum, stroke/shadow, placement, movement and
named contexts — captured concrete through the Theme Editor's resolution
fixture (the service current theme excluded), with contexts resolved against
that same base and unknown names retained. No service ids, media paths or
backdrop fields travel. The strict deterministic codec shares one semantic
sentence (`Documents::validThemeSnapshot`) with the coming catalog loader, so
the two cannot drift. `domain_tests` covers round-trip/determinism, Unicode,
effects/layout/movement/contexts, silent-theme defaults and every refusal.
PT.2 added catalog format 2's optional remembered look and the adapter surface
that uses it. A real preference change advances both revisions and leaves the
canonical words untouched; re-setting it is a no-op; Library undo/redo owns it
and it survives restart. The Library's change signal is separate from content
so remembering a look cannot re-prepare a linked show. Capture, measurement
(`boxStyleForSnapshot`) and creation-only application make a new service
preparation wear the saved look, while a lyric refresh keeps the item's
existing look. A→restart→B is tested with a colliding local theme and with A's
theme edited and deleted.

PT.3 added the explicit whole-item assignment in service 15. An applied item
theme replaces the service current theme and the target's old text/layout
overrides and slide themes, while backdrop customizations, words, IDs, breaks
and media stay; a later per-slide Edit choice still wins. "Use service default"
returns just that item to the legacy chain. Both verbs are one history entry,
appearance and transition share the branch, and format-14 files naming the new
key (or files applying an unknown theme) refuse by name.
PT.4 added the bounded reflow source. A show stores its source kind, ordered
section/verse blocks with original-case lines, scripture provenance and a
fingerprint of the text projection the slides currently show. Any word, break,
order or section edit invalidates the match; style-only edits do not; undo/redo
and save/reopen carry both. Song preparation/refresh and scripture Add capture
their own source, and a conservative current-word fallback covers legacy or
edited items without consulting the Library or a provider (unlabelled scripture
blocks name the whole passage rather than guessing verses).
PT.5 added the eligibility sentence and the preparation. Reflow is offered only
for a run of simple text slides with one box each, common background/fit/context
and at most one leading blank cue; every other structure refuses with a specific
reason. A plan measures the item's own words (stored source or the conservative
fallback) in the chosen look's box through the worker, using the song or passage
policy, and is refused if the words moved while it was measured. Cancelling,
failure and supersession never touch the document, and Apply only remains
available without a measuring worker.
PT.6 added the apply transaction. A prepared plan is revalidated against the
item's current words, cue, slide count and media before anything durable
happens; the Library preference is written through Library history; then one
compound service edit publishes the look and the replacement slides. One
service undo restores the old theme, slide IDs, words and common media; one
Library undo forgets the look without touching the service. Refusals and stale
plans publish neither side, and cross-store crash atomicity is not claimed.
PT.7 moved the apply entry point to left-hand song/passage rows and added the
one keep/reflow dialog: target title, occurrence count, theme, eligibility
reason, Apply only (default), Apply and reflow (honest no-worker refusal) and
Cancel, plus Remember theme for an unprepared Library song. Service rows also
offer Use service default. The cue-tile theme menu and the header's separate
default-theme picker are gone; per-slide styling stays in Edit. Both the mouse
and the Menu/Shift+F10 keyboard path reach the row menus.
PT.8 closed the journey: a look saved in service A is worn by a later service
B, Apply only preserves IDs/backdrop/common media while removing text
overrides, Apply and reflow keeps every current word and goes on air through a
real Go, save/reopen keeps the look/source/words, legacy scripture falls back
to its current words, and the two undo scopes stay separate. `docs/architecture.md`
gained the PT model/interaction section; the PT brief records the
implementation instead of its planning-only banner.
Ledger: PT.1–PT.8 DONE; desktop 88/88 on the clean runs, headless 48/48
(display excluded), workspace at scale 1.5 clean. Two later `-j8` re-runs
flaked the known timing-sensitive `worker_rpc_transport` and
`background_media_tests`; both passed alone and in neighbouring full runs. Physical gates (projector, NDI, target GPU)
remain open under M7/ND/TA and are not claimed.
Next action: await the next group (TA.1, TV leftovers, ND, FO.1–FO.6,
M7.1a–7.4 remain open in `PLAN.md`).

**Edit canvas follow-up (BX.5, 2026-11-16): fixed and verified.** Sustained
box drags could be stolen by Edit's parent Flickable; the earlier pointer matrix
used one jump per drag and missed this. The canvas now keeps accepted box grabs,
while empty-space presses remain available to the page. Centre guides now use
their fitted-content parent's origin instead of applying the stage offset twice.
The revised pointer matrix reproduced both failures before the fix, and checks
guide endpoints as well as visibility. Build of `openshow-ui` and
`workspace_ui_tests` passed using `CCACHE_DIR=/tmp/openshow-review-ccache`.
Focused ctest checks passed **3/3** (workspace 63.52 s, control set 3.02 s,
UI load 8.02 s); the workspace binary also passed with `QT_SCALE_FACTOR=1.5`
and `2` on the offscreen/software backend. `git diff --check` passed. Restart
the application to load its rebuilt QML; no physical-display check is claimed.
Prior BX.1–BX.4 evidence below remains historical.

**Box manipulation (2026-11-16): BX.1–BX.4 are DONE.** The
[box-manipulation brief](docs/ui/box-manipulation-plan.md) fixes all four corner
handles across Theme Songs/Bible/Edit and removes numeric geometry/Center H/V
sections. Body drag retains centre snapping; placement reset/borrow moves to
box context menus. TL/DR/PV completion remains historical evidence, not BX
acceptance.

BX.1 added `BoxTransform.qml`, the one geometry and hit-test implementation
both canvases instantiate: centre snap, all four corner hit tests with a
nearest-centre rule, body move with preserved size, and the four-direction
resize that moves both edges the corner owns while fixing the opposite corner
and clamping to the 20-unit minimum and the canvas. `EditCanvas` and
`ThemeEditorPage` now delegate to it; bottom-right changes both dimensions (the
reviewed code left it inert) and a release equal to the captured rectangle
writes nothing, so selecting a box adds no undo step. Corners and bodies carry
the right cursors. `control_set_ui_tests` proves the helper matrix;
`workspace_ui_tests` proves both pages over one captured rectangle.

BX.2 made the handle areas reachable and the gesture owned. Both gesture
surfaces extend one handle radius past the fitted content (mapping coordinates
back by the inset), and the Themes overlay no longer clips its handles, so the
outer half of a corner at a slide edge is live. A gesture captures its target
by identity at press (theme + view; slide + box id) and writes only if that
identity still holds and the rounded rectangle changed. Navigation, a
selection/theme change, a view switch, deletion and Escape cancel and restore
the model geometry; a held drag never retargets a new box. Selected handles beat
an overlapping body; body presses still select the topmost item. Desktop suite
85/85.

BX.3 removed the numeric geometry UI: `BoxGeometryControls` is retired, the
Songs/Bible/Edit rails carry no X/Y/W/H, Centre H/V or owner/placement rows,
and Edit keeps only its stack commands and keyboard tools. Placement reset and
the Bible borrow moved to each canvas's box context menu (right-click or
Menu/Shift+F10), routed through the existing adapter/domain verbs so siblings
survive and a reset is one undo; opening a menu creates nothing. The Edit band
and Theme rails were re-laid out with no blank spacer where the section sat.
`workspace_ui_tests` pins the absences, the menu paths, reset/borrow effects and
the surviving TL/stack controls; `inspector_adapter_tests` pins field
preservation and save/reopen. Desktop suite 85/85.

BX.4 proved the interaction with real pointers and on the worker. The Edit
matrix drives actual press/move/release through every corner at 1000/1380/1680
inward and outward, both centre axes and their intersection (exact 960/540 with
both guides), a beyond-tolerance drag, a small rectangle's body move, an edge
corner, the selected handle over an overlapping box, Escape and one undo. The
Themes overlay runs the four-corner matrix in Songs and Bible against the record
each view owns, then the centre guide, edge corner and Escape. The worker
journey moves and narrows a live cue's box, shows the off-air picture while the
audience keeps its frame, publishes matching pixels on an explicit Go over
continuous media, and keeps the rectangle through save/reopen; reset/borrow
sibling preservation stays pinned by `inspector_adapter_tests`.

Verification: desktop **85/85** (100.9 s, 21 display entries); headless
**47/47** with display excluded (15.6 s), source guards included; the workspace
binary passes at `QT_SCALE_FACTOR=1.5` and `2` with zero failures; the populated
capture path produced 24 images at each of scales 1/1.5/2. No display-labelled
test was skipped here. Physical M7/ND/TA gates (projector second display, NDI,
target-GPU checks) remain open. BX is closed; TA.1 and the M7 gates remain as
listed in `PLAN.md`.

**Text layout and editor consistency (2026-11-16): TL.1–TL.6 are DONE.** The
agreed
[text-layout brief](docs/ui/text-layout-plan.md) adds shared vertical-alignment
icons and All caps to Theme Editor/ Edit, with theme defaults and per-slide
overrides. Preview updates immediately; audience changes on explicit Go.
Original-case words are preserved; worker measurement/rendering share Unicode
uppercase. The scoped consistency review includes underline-only reset state.

TL.1 added sparse `verticalAlign` and `allCaps` to themes, named text contexts
and slide customizations, resolved slide → context → nearest theme → default,
and moved the service to format 14. The silent pair resolves to centre / normal
case, so legacy files keep their pixels and IDs; an explicit `allCaps: false`
survives as an authored answer. Pre-14 files naming either key are refused by
name and era, and an off-list anchor is refused by the domain's own sentence.
`domain_tests` covers the chain, storage round-trip, explicit false, context
precedence, clearing and refusals; the new `document_controller_tests` fixture
`exerciseTextLayoutSurfaces` covers the theme row's `...Set` pair, whole-set and
one-field verbs with one undo each, the Edit page's customize/revert badges,
the context read model, duplication and `createThemeFromCurrent`, and the
save/reload round-trip.

TL.2 carried the pair to the wire: scene format 8 writes `verticalAlign` and
`allCaps` on every text item (source words keep their authored case), scene-7
payloads read with centre/normal-case defaults and are refused when they name
either key, and protocol minor is 20. `text.measure` accepts the optional pair
under v3 and refuses a malformed one by name. `scene.text-layout.v1` and
`text.measure.v3` are defined but deliberately withheld from this build's hello
until TL.3 earns them; the frontend gates a non-default pair by name while
centre/off content keeps the legacy payload shape. The shared inspector exposes
both leaves on themes and slides and the one style pair a Bible view already
stores, with provenance, commit and reset.

TL.3 taught the shared layout engine to draw and measure the pair. One
`verticalOffset` calculation divides the free space 0 / half / all for
top/centre/bottom, and paint, ink bounds and fit diagnostics all read it, so an
overflowing fixed-size paragraph stays top-anchored as before. `displayLines`
applies ICU's root-locale uppercase once, before shaping, preserving authored
line breaks and empty lines and leaving the source strings untouched; the
transformed result is checked against the scene's line/item/scene budgets with
a named refusal, and `scene.prepare` runs that guard before the store grows.
`text.measure` enters the same path, and an ask that names an anchor with a box
height gets positioned bounds while the intrinsic height stays put. The worker
now advertises `scene.text-layout.v1` and `text.measure.v3`.

TL.4 made the pair part of the planning question. `FlowStyle` and
`MeasureRequest` carry it, the packer forwards it, and both the measurement
cache key and the song fingerprint name it, so a toggle is a different plan
rather than a stale hit. The box-style read comes off the same compiled
`SceneItem` the added slide will wear, and both importers put the pair on every
measurement block. The pages already regenerate on `documentsChanged`, so a
toggle re-measures the held words with no second network request; the planner
clears its plan immediately and the Bible page now discards a plan whose inputs
moved mid-measurement, so a delayed plan can never enable Add. Source words and
labels stay original-case, and committed slide boundaries are untouched.

TL.5 added the shared controls: a Vertical row of exactly three in-repository
paragraph marks beside the horizontal alignment row, then one All caps
checkbox, in the same order on Songs, Bible and Edit. Icons carry accessible
names and tooltips, take focus by Tab and activate by Space/Return; every row
shows the resolved value, the own/inherited dot and the same reset chip.
Remove customizations is now driven by the domain's sparse set
(`currentSlideCustomized`), so an underline-only edit and an explicit `false`
both wake it — the old QML name list omitted `fontUnderline`. Every gesture is
one inspector commit (one undo), the off-air preview re-asks on
`documentsChanged`, and no edit presents.

TL.6 closed the integrated journey and the documentation. `production_journey_tests`
drives the live gold-backed cue through the DocumentController the Edit page
binds: a top anchor and capitals are authored, the off-air `preview.select`
frame shows the words moved up while the audience keeps its acknowledged
centre-anchored revision (the leak counter stays 0), an explicit Go presents
the prepared revision and the acknowledged audience pixels move to the top
anchor with the capitals drawn over the same gold asset, save/reopen keeps the
pair and the source words' authored case, and two undos return the cue to
centre/off with the media identity unchanged. `frontend_journey_tests` proves
the same preview-only rule through the production `WorkerController` rather
than raw RPC: `requestSelectionPreview` moves the off-air picture while the
audience revision and frame do not move, and only `presentSceneJson` puts the
edited anchor on air. `docs/architecture.md` gains the text-layout section
(chain, formats, capabilities, the one-origin renderer, the root-locale
transform and its budget guard, measurement/planning, the shared controls and
the evidence list), and `docs/ui/control-sets.md` records the Vertical/All caps
rows, the `glyphStyle: "vertical"` segments with keyboard activation and
accessible names, the new object names, and the layout pair's place in the
context merge and the Remove-customizations state.

Verification for the group: desktop suite 85/85; headless suite 58/58 (display
excluded, including the Qt-free, Skia-public-API, render-layer and
ui_loads guards); the desktop run also carries the negative source guards.
Physical gates (audio hardware, target-GPU NDI/live output) remain the standing
M7 items, and no TV.3 effects or per-box styling are claimed. DR/PV completion
below remains unchanged.

**The design-conformance corrections (DR.0–DR.8) are complete.** The review
[`docs/reviews/design-conformance-2026-11-15.md`](docs/reviews/design-conformance-2026-11-15.md)
found seven integration regressions in the media work that landed with
`054a1d0`. Each is fixed, with the regression pinned by a test that fails on the
reviewed commit:

| Leaf | Commit | What changed |
|---|---|---|
| DR.0 | `9f146fe` | The cue audio contract is recorded in `docs/architecture.md` ("Media audio: one contract, and when it restarts"): three lifetimes (cue layer + embedded video audio in one player; legacy `media.standalone`), the audible identity as the pair (embedded stream, show layer), continuity across slides and visual-only changes, Clear Background/Slide preserving the layer, Clear All stopping it, Restore starting it from the beginning, seek/loop on the player, mute/gain surviving rebuilds, and acknowledged show audio counting as output activity. |
| DR.1 | `7351d9b` | `withBackgroundCleared` is a complete transformation through the scene contract (video/camera/source/mirror/gradient all go), and `sceneContent` derives its flags from the same typed reader the worker uses. A video clear used to be refused by the codec; a camera clear used to come back on air. |
| DR.2 | `e408181` | One traversal (`SceneEdit::visitAssetPaths`) covers the background, every image item AND the scene 7 audio layer; the mirror, pending prepares, the Restore snapshot and the desired presentation all adopt a save's moved paths. |
| DR.4 | `da1960b` | The staged player and every flag describing it are one `StagedAudioBundle`, taken whole under one lock — no loose cross-thread booleans. A barrier test publishes B inside the take-hook window and proves A keeps its own identity. |
| DR.3 | `09be595` | Audio is staged for every background kind (audio-only, still, camera, video) and keyed by the complete identity; the on-air shortcut includes the layer; a scene whose prime is in flight defers once; a staged failure rides the bundle; a visual-only change keeps the player and its position. |
| DR.5 | `80c8259` | `SceneContent.audio` / `audioOnAir` count the cue's own layer as output activity for the panel's label, Clear All and the Escape gate, while words and background clears stay layer-scoped. |
| DR.6 | `f2ba7e3` | Plans capture and recheck catalog record/content revisions and the destination show; owed ids live in a deduplicated, catalog-bounded set drained one run per publish; a removed destination is dropped, never re-added. |
| DR.7a | `7864058` | `media.describe`/`media.thumbnail` run on their own bounded, cancellable executor and answer by deferred reply, so a slow bin file cannot hold Go/clear/status. |
| DR.7b | `afcee65` | Bin pixels are a worker-produced PNG in a bounded cache (64 files / 32 MB / 24 h, name fingerprints source mtime+size); the reply carries the path, the card loads a file URL, and a reply is installed only for the request this client made. |
| DR.8 | this commit | Journeys and suites re-run, architecture reconciled, superseded handoff narratives archived. |

**Verification (DR closure, 2026-11-16):**

- `cmake --build build --parallel 24` — clean.
- `ctest --test-dir build --output-on-failure` — **85/85 passed** in 474 s,
  including the 21 `display`-labelled entries, the 10 architecture/dependency
  guards and their negative fixtures (`skia_public_api_guard`,
  `render_layer_guard`, `render_target_guard_negative`, `app_has_no_ndi_symbols`,
  `ui_links_qt`, `worker_check_deps`, `ui_loads_guard_*`).

**Verification (PV closure, 2026-11-16, after the 30 fps ask and the per-frame
repaint):** `cmake --build build --parallel 24` clean, then
`ctest --test-dir build -j8 --output-on-failure` — **85/85 passed**, 274 s of
`display`-labelled test time across the same 21 entries. `worker_controller_tests`
(66.5 s) and `async_integration_tests` (45.5 s) carry PV's own legs.
- `cmake --build build-headless --parallel 24` then
  `ctest --test-dir build-headless -LE display` — **47/47 passed** in 81 s.
- Skips are the environment's, not the suite's: PipeWire-daemon and VA-API
  branches report internal skips when the sandbox denies the audio server or
  the GPU decoder, and `media_tests` skips its PipeWire check without a daemon.
  No test was skipped by label beyond the headless run's explicit exclusion.

## Preview cadence (PV)

PV.1 is DONE (2026-11-16): the frontend asks for 640×360 at **30 fps** on every
hello, and `PreviewConsumer` raises `changed()` on every accepted copy of any
target, so the panel repaints per frame instead of on the 500 ms status poll.
`worker_controller_tests`' `scenarioPreviewCadence` records the ask verbatim on
two connections (the first dies after answering `preview.start`, so the second
is a real reconnect) and requires the `previewChanged` count to cover the copies
the 20 ms-beat producer delivered; reverting the `fresh` fix fails that line.
The worker's own `preview.start` default stays 15 fps — only the frontend's ask
changed, and the cadence is a ceiling drawn on content change.

PV.2 is DONE too, measured on this machine on 2026-11-16 rather than claimed:
Wayland, RADV **AMD Radeon 890M** (STRIX1), 1920×1080 swapchain, h264 decoded in
software into Vulkan linear images, previews 640×360 in three slots, audio clock
live through PipeWire. The bench grew `--preview-fps` and delivery counters
(`ab159b0`).

- 60 s with a continuously changing **1080p60** source (ffmpeg `testsrc2`
  1920×1080@60 plus a 440 Hz tone, looping), previews asked at 30 fps, no off-air
  requests: **1795 frames consumed in 60.0 s = 29.9/s**, published 1795,
  **dropped 0, deferred 0**, worst preview frame 2.45 ms — above the 28.5/s
  target. The same run presented the 60 fps source to the audience at ~60/s with
  93 frames later than 25 ms (2.6%, drift max 46.9 ms); that is a 60 fps
  audience question, not the gate below, and it is recorded rather than hidden.
- Ten-minute gate with previews at 30 fps on the existing 1080p25 fixture:
  `presents=15000` (exactly 25.000 fps), **missed(>25 ms)=0 (0.00%)**, drops 0,
  drift mean +2.4 ms / max 16.3 ms. Preview consumed 15000 in 600.0 s = 25.0/s
  with **dropped 0, deferred 11, worst 15.92 ms** — the rate is the source's own
  change rate (30 fps is a ceiling), and the deferrals are the audience-budget
  policy standing down after the worst frames, as designed.
- "Visible UI updates" was verified through the product objects, not an eye:
  `async_integration_tests` leg `uiRepaintsWhileContentMoves` requires the
  `previewChanged` count to cover the frames the frontend accepted from a 30 fps
  clip over three seconds, and fails when PV.1's `fresh` fix is reverted. No
  human watched the panel during these runs; that one clause is not claimed and
  the projector/physical checks keep their existing owner.

The measured runs met the target, so no asynchronous-readback follow-up was
opened and the audience budget was not raised.

**Still open (unchanged by DR, and not inferable from a green suite):** TV.3,
TV.4a, TV.4b, TV.5 and TA.1; ND.4's real source-loss acceptance; M7.1a–c,
M7.2–M7.4; the physical acceptance that needs the projector on a second display
(Wayland and X11), a live camera unplug, an audible pairing and a long mixed
service. FO stays gated behind those.

## How to pick up

1. `PLAN.md` is the contract; the DR group is closed and the remaining open
   leaves are named above. `docs/architecture.md` carries the reasoning the
   code is held to, including the DR.0 audio policy.
2. Build: `cmake --build --preset desktop --parallel 24` (`build/`); test:
   `ctest --preset desktop -j8` (~8 min). Headless: `--preset headless`
   (`build-headless/`, 47 entries, display excluded).
3. Facts worth keeping: a ctest "Passed" after a failed build means a stale
   binary — rebuild and grep for `error:`. `worker_controller_tests`,
   `rpc_ipc_tests` (`worker_rpc_lifecycle`) and `live_fade_tests` take the
   worker binary as argv[1] under ctest. Test fixtures are written into the CWD
   (`transport-fixtures/`, `audio-mix-fixtures/`, `playback-fixtures/`,
   `background-media-fixtures/`, `scene-render-*.png`) and are git-ignored.
4. Protocol/service/scene formats are now **protocol minor 21**, **service
   format 15**, **scene format 8**; `docs/architecture.md` and
   `docs/ui/control-sets.md` are the authorities for the wire and the controls.
   TL.3 earns `scene.text-layout.v1` and `text.measure.v3`; EW.3 adds
   `text.editor.v1`, so the worker's hello now names all three.

## Recent commits

- `8b2f2d4` fix(ui): announce capabilities after the hello reads them
- `2f94500` fix(ui): re-read the slide's boxes when navigation changes (EW.5)
- `b1c7674` docs: record the clean EW verification run and the skipped font fixture
- `38880fb` fix(worker): refuse a superseded text.editor request (EW.3)
- `89151b4` feat(ui): edit words on the stage and finish the EW group (EW.4–EW.5, EW.9–EW.10)
- `ece6e64` feat(worker): answer editor caret and selection geometry from the real layout (EW.2–EW.3)
- `7b23b96` feat(ui): freeze the WYSIWYG contract and add slide move/insert with a real rail (EW.1, EW.6–EW.8)
- `1487777` test(ui): cover the remaining pointer fixtures and raise the suite wall (BX.4)
- `6c7c384` fix(ui): map inset gesture coordinates and fit the stage for edge handles (BX.4)
- `e38bae9` test(journey): publish a moved box through preview, Go and save/reopen (BX.4)
- `c34160b` test(ui): drive the Themes overlay with real pointers in both views (BX.4)
- `630574c` fix(ui): read gesture coordinates from the inset surface and keep handles in bounds (BX.4)
- `37b1a47` feat(ui): remove numeric geometry and move reset/borrow to box menus (BX.3)
- `85a3495` fix(ui): make handles reachable and gestures identity-owned (BX.2)
- `6ae2666` fix(ui): share box geometry and resize every corner (BX.1)
- `332a77b` docs: plan the BX direct-box-manipulation group
- `cd52b37` test(journeys): close the layout journey and document the controls for TL.6
- `f2fb8f3` feat(ui): share the vertical/anchor controls and fix Remove customizations (TL.5)
- `64ae726` feat(importer): plan with the resolved layout pair for TL.4
- `6a14f9e` feat(worker): render the anchored layout and root-locale capitals for TL.3
- `e02af41` feat(scene): carry the resolved text layout to the wire for TL.2
- `f57e5b2` feat(domain): persist and resolve the text-layout pair for TL.1
- `a652e1c` docs: plan the TL text-layout and editor-consistency group
- `7d4631d` test(frontend): keep the panel repainting while the picture moves (PV.2)
- `ab159b0` feat(worker): measure preview delivery at a chosen cadence in the A/V bench (PV.2)
- `6fedd59` feat(ui): ask for 30 fps previews and repaint on every accepted frame (PV.1)
- `afcee65` fix(media): deliver Media Bin pixels as a worker-produced cache file (DR.7b)
- `7864058` fix(media): run Media Bin probe/decode work off the control connection (DR.7a)
- `f2ba7e3` fix(library): every linked refresh survives a delayed preparation (DR.6)
- `80c8259` feat(ui): count the show's audio layer as output activity (DR.5)
- `09be595` fix(media): stage audio for every background kind and keep it across visual changes (DR.3)
- `da1960b` fix(media): transfer the staged player and its metadata as one bundle (DR.4)
- `e408181` fix(scene): adopt every scene asset path, including the show audio layer (DR.2)
- `7351d9b` fix(scene): clear media backgrounds through the typed contract (DR.1)
- `9f146fe` docs(media): record the cue audio contract and its continuity rules (DR.0)
- `a10ed4b` docs: review design conformance and plan corrective work
- `054a1d0` feat: audio is a cue, camera mirroring, and the type/effects review
