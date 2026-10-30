# OpenShowQt progress and handoff

Updated: 2026-10-30 (Europe/London).

## Current checkpoint

**M0–M5 are historical implementation checkpoints. M6 is incomplete.**
The committed checkpoint is task **6.20b**, the Skia-guard re-read and the
RPC/media audit update (6.20a's evidence matrix at `ebb282d`,
6.19b8's `SettingsPage` at `88cb050`, completing the 6.19b split,
6.19b7's `EditPage` at `f6752b0`,
6.19b6's `ShowPage` at `8722b9e`, 6.19b5's `PlaylistPanel` at `cfc782c`,
6.19b4's `OutputPanel` at `5446a6e`,
6.19b3's `ShortcutMap` at `337626e`,
6.19b2's `DocumentViews` at `eaf5586`,
6.19b1's `DocumentPersistence` at `7050a2d`,
6.19a4's worker `RpcMethods` split at `7cd5edf`, which completed 6.19a,
6.19a3's `PresentationModel` at `c00d025`,
6.19a2's `PreviewConsumer` at `a4cb9e2`, 6.19a1's `RpcClient` at `001588e`,
6.9b's prepared-scene prewarm at `d83469e`, SceneStore's one scene truth at
`1b54bdb`, and the `PreviewProducer`,
`BackgroundTransport` and dev-binary extractions plus the bounded prepare
executor in the commits before those). The R10 scheduling pair is now closed:
prepare-time file work left the RPC dispatch loop on 2026-10-29 (**6.9a**) and a
prepared scene's media, text and blur readiness left the render loop on
2026-10-30 (**6.9b**). The authoritative task/status ledger is
[PLAN.md](PLAN.md#m6--code-review-remediation-and-architecture).
Productization, recovery, packaging and full hardware acceptance remain M7.

**Task 6.9b is committed** (`d83469e`). A background clip's bring-up is a PRIME:
`PlaybackPipeline` split into `openFile` (probe, format, decoders — any thread)
and `attachGpu` (uploader + ring — render thread), `BackgroundTransport::primeVideo`
runs a prepared clip's open on the prepare executor and parks exactly one prime
under its own leaf mutex, and `applyBackground` adopts that prime at the frame
boundary instead of opening the file there (a cold open is still honest when
nothing was primed or the attach fails). A prepare also prewarms the two caches a
draw reads: the renderer's text layout — which moved from the shared
`SceneTextEngine` to each `SceneRenderer`, because the companion borrows the
engine and R16's isolation means a churning slide list must not evict the
audience's layouts — and a blur background's backdrop. `status().scene.images`
gained the trio `drawDecodes`/`drawBackdrops`/`layoutBuilds`: what the frame loop
still had to build for itself.

**Task 6.19a1 is committed** (`001588e`). The frontend's transport left
`WorkerController`: `src/ui/RpcClient.{h,cpp}` owns the `QLocalSocket`, the
shared framing, the ledger of requests the worker owes, the size/backlog/
in-flight refusals and the 2 s no-byte write deadline, and it interprets nothing
— a reply comes back with the method its request was sent under, the connection
generation it was written on and the sender's own intent record, so R3's
correlation rule now lives in exactly one class and a new protocol method cannot
drift from it.

**Task 6.19a2 is committed.** The frontend's pictures have left
`WorkerController`: `src/ui/PreviewConsumer.{h,cpp}` owns the
shm segment `preview.start` names (mapping it on the first ready, retiring it
when the stream does), the audience mirror, the editor's off-air target and the
slide list's thumbnails — both generation counters, the asked/wanted dedupe, the
byte-bounded LRU and the status-poll stand-downs that re-issue an ask the worker
no longer holds. It reaches the worker through three callbacks its host injects
(request, notification, and the same 6.8a scene-budget predicate a present is
checked against, so the rule still has one owner) and interprets no reply beyond
its own methods, the `preview.ready` frame and the `preview` block of
`output.status`. The `scene.prepare` intent both preview asks ride on is now the
shared `SceneAsk.h` record the session and the consumer both name, which is what
keeps a prepare ack from answering the wrong feature. `WorkerController` keeps
the process, the handshake, the poll rhythm and the acknowledged output model,
and re-exports the picture properties unchanged for QML.

**Task 6.19a3 is committed.** The acknowledged output model has left
`WorkerController`: `src/ui/PresentationModel.{h,cpp}` owns the revision mirror
(the payload each prepare stored, bounded to the same 32-revision window the
worker prunes to), the latest-go rule that lets only the newest go's
acknowledgement move the audience, the pending logical presentation the clears
compose against, the restore snapshot with its confirmed-on-air retirement, and
the asset-path adoption that rewrites every payload the model holds plus every
prepare still awaiting its ack. It sends through injected callbacks — the live
gate, the same 6.8a scene-budget predicate the preview asks already pass, the
request sender, the prepare intent attached after the send (R3's one-record
rule), the visit over in-flight prepares and the status line — and interprets
nothing but the prepare ack, the `scene` block of `output.status` and a
refusal's in-flight marker. `WorkerController`'s six scene properties, its go,
clear, restore and adopt invokables forward; the QML surface is byte-identical.

**Task 6.19a4 is committed**, which closes 6.19a. On the worker side
`src/worker/rpc/RpcMethods.{h,cpp}` now holds what a request MEANS — every
method's argument decoding in the width the value is carried in, its refusal
texts, its result encode and the session call it names — together with the four
encoders that describe session state on the wire (`output.status`,
`output.displays`, `preview.ready`, `media.error`). `RpcServer` is the
connection: accept, framed read, the `hello` gate, the bounded coalescing queue
and its progress-measured write deadline, the drain, the deferred-answer bag
with its self-wake and the shutdown. The table cannot end a session and the
connection decodes no method; `scene.prepare`'s deferred ack comes back through
an injected sink, so the executor thread never learns a socket exists. Names
and shapes stayed in `openshow/protocol`, so the split added no vocabulary for
frontend and server to drift over, and `RpcServer.cpp` is 909 lines shorter.

**Task 6.19b1 is committed.** Durability has left `DocumentController`:
`src/ui/DocumentPersistence.{h,cpp}` owns the `AutosaveScheduler` and its
thread, the task 2.3 startup rule (a scratch is newer than the saved service and
so wins, and recovering one is why the session starts dirty), the refusal
surfacing that keeps the crash net while landing the warning deferred so it is
the last word the operator reads, the decision-epoch protocol that gives a save
or a load the scratch alone, the removal of the scratch a decision superseded
with the survivor wording that silent resurrection would cost, the quit-edge
durable wait and the executor counters. It holds no documents and no history: a
save takes the documents by pointer because the domain's adopt step retargets
each asset at the package's copy, and hands the rebinds back so the controller
can tell `History` — the state that matters is the one `save()` returns with, in
which no snapshot names a deleted path (R7); a load returns separate documents
for the controller to move into its own when it is ready to give up what it had.
The controller keeps the documents, the history, navigation and the dirty flag,
and its whole persistence surface — `save`, `load`, `startBackgroundPersistence`,
`awaitPersistence`, `prepareForQuit`, `autosaveWrites`, `autosaveCoalesced`,
`serviceDirectory` — is unchanged, which is why no test was edited.

**Task 6.19b2 is committed.** The row shapes have left `DocumentController`:
`src/ui/DocumentViews.{h,cpp}` spells what the operator's lists read — the
song rows, a song's slide rows (first-line preview, the first box's geometry,
background asset and fit), the current slide's box rows, the style list and the
current slide's resolved style with its gradient, the playlists and their
entries with each referenced show resolved, the media registry, the drawer's
search projection, and the selected box's text, kind and asset. It is a pure
reader: a const reference to the controller's documents, no state, no signals,
no cache, so a row cannot be one edit behind the model. Where a shape depends
on WHERE the operator is, the navigation arrives as a `Cursor` value the
controller builds from the ids it owns — the views cannot hold a stale position
because they hold no position at all. `selectedBox()` is the single resolution
of the selection with its fallback to the bottom of the stack, and the
controller's own box-scoped edits resolve through that same call, which is what
keeps R17's "the box wearing the handles is the box the inspector edits" a
property of the code; `linesAsText` is the one conversion the inspector's text
and the debounce comparison both read, so they cannot disagree. The controller
keeps the documents, the navigation and every mutation, and all of its property
getters forward — no QML binding, row key or fallback changed, which is why no
test was edited. `DocumentController.cpp` is 261 lines shorter.

**Task 6.19b3 is committed.** The window's whole key table has left
`Main.qml`: `src/ui/ShortcutMap.qml` carries all 49 `Shortcut` objects — the
Escape/F1/F2 output keys, the Ctrl+D duplicate-or-toggle and Ctrl+F search
rules, Ctrl+1..5 for pages, the 1px/10px nudges, Tab/Shift+Tab stack steps,
the presenter keys and the opt-in number keys, undo/redo/save/clipboard and
F11 — and computes their gates itself from eleven fact properties: which page
is up, what holds the focus (typing, a squatted button, the drawer, the
canvas's handles), the on-air state, and the settings opt-in. Every activation
emits an intent signal; the window handles them beside the same buttons that
perform the actions, and keeps only the facts its own `activeFocusItem` can
speak for. No key changed owner, sequence or gate on the way out — the two
Escape owners still hand off on the canvas selection, the Ctrl+D pair still
splits on slide count, the presenter arrows still stand down for a nudging
box — and that is the acceptance: two enabled `Shortcut`s for one key make Qt
drop the key entirely, so the R17 rule that each key has exactly one live
owner is now something one file can be read to check. `Main.qml` is 240 lines
shorter.

**Task 6.19b4 is committed.** The right dock has left `Main.qml`:
`src/ui/OutputPanel.qml` is the `SidePanel` and everything inside it — the
shared-memory preview and its stats line, the monitor picker bound to the
worker's applied display, the layer toggles, the demo-scene row, the
per-layer clears with the Clear-all/Restore toggle gated on the worker's
acknowledged live flags, the media transport and mixer rows with their
at-rest-only slider re-application (a 500 ms poll must not yank a drag), the
standalone track row, and the device/frame/revision readout with its guarded
display-name lookup. What split wrote as "explicit properties for what the
window owns" turned out to be no properties at all: the dock reads only the
`worker` and `panels` context properties — unlike the shortcut map it has no
window state to ask for and emits no signals — so the window keeps exactly
what is genuinely its own: the anchors placing the dock between top bar and
drawer, and the `rightDock` id the two centre pages anchor their right edges
to. The window's private inline `PanelField`/`PanelButton` (task 4.4) became
module files so the extracted panels carry the same look without borrowing a
window declaration. `Main.qml` is 367 lines shorter; the live notice set is
the same five, both `Flow` complaints now naming `OutputPanel.qml`.

**Task 6.19b8 is committed, and with it the 6.19b split is complete.** The
settings page has left `Main.qml`: `src/ui/SettingsPage.qml` carries the two
persisted toggles over `appSettings` (number-keys presenting, F11 fullscreen),
the fifteen-row shortcut cheat sheet, and the service rows — version with the
worker's status, the service directory, the autosave explanation. The row's
specific requirement was that the cheat sheet state the keys
`ShortcutMap.qml` now owns: every entry was checked against the map (all
fifteen match their owners and sequences), the page header now names the
mirror duty in both directions, and `ShortcutMap.qml` gained the matching
pointer — which shifted its three live multi-sequence notices from lines
92/289/304 to 96/293/308: the same three notices, still the whole extra
output. No properties, no signals — everything it reads is a context property,
like the two docks before it, the fifth time the split's rows overestimated
what the window genuinely owns. `Main.qml` is 255
lines: placement, which page is up, the focus facts, and the executors the key
map's intents land on. Where the split guessed at properties and signals five
times, the real blocks needed them zero times — the window's remaining
ownership is placement, the facts only its `activeFocusItem` can answer, and
presenting.

**Task 6.20a is committed.** `docs/architecture.md` gained
"Review closure: R1–R19 evidence matrix": one row per finding, naming its
remediation tasks, the fixtures that pin it with the preset that actually
registers each (**H** = in the 32-test headless inventory, **D** =
desktop-build only, **D✦** = display-labelled), and its open remainder. The
mechanics were verified, not assumed: the two preset inventories were listed
directly (`ctest -N` for each, the single-digit alignment in that output
cost an undercount the first pass) and every fixture in the matrix exists in
its stated inventory; both trees were unchanged when the leaf's gates ran, so
the green run the matrix cites is the same code it describes. What the matrix
found bare it recorded bare rather than absorbed: per-item foreground-image
reporting and the two architectural wordings to 6.20d, the reliability set to
M7.1a–c exactly as the review mapped it, the accounting and budget items to
M7.2, and the cheat-sheet alignment to what it is — verified by reading both
files, no automated keyboard test existing. The table's foot names the
completion rule's three remaining running obligations at their leaves:
6.20b, 6.20c, 6.20d.

**Task 6.20b is committed.** The public-Skia re-read found a real gap:
`CheckNoSkiaPrivate` matched only quoted includes, but
`cmake/OpenShowSkia.cmake` exports the Skia *root* itself (the modules'
public headers are reached as `<modules/<m>/include/...>`), so
`#include <src/core/SkYUVMath.h>` resolved at compile time while the guard
smiled. The regex now refuses `src/`, `include/private/` and
`modules/<m>/src/` in both quote styles, and the negative fixture grew
`violation_angle.cpp` so the guard stays proven to fail on both forms
(manually confirmed: positive green over the 144 scanned files, negative
flags both and exits 1). Everything else the re-read checked held: the glob
is directory-scoped so every module added since 6.2a is covered without a
file list, the exemption list is empty, and the exported paths 6.2a narrowed
are still narrowed. The review's RPC/media audit table now carries the
enforced numbers under a dated revision marker, with the review-time claims
left visible above it: 1 MiB in both directions with the sender refusing
locally, depth 8 against the measured legitimate 5, the scene budgets
8/256/1024/16 KiB/64 KiB/512 and the `data:` refusal, method 64 B / echo
160 B, the 1 MiB / 1024-frame outbound queue, in-flight 64, preview
negotiation 64..1920 × 48..1080 @ 1..60, and the separately bounded
on-demand targets. The still-true findings stay stated: no media-byte or
data-URI method on the wire, and the surviving render-thread readback limit
remains an open-differences row with its measurement at M7.2. One naming
correction recorded in the ledger row: the split's `rpc_limits_tests` does
not exist — the fixtures are `protocol_tests`, `scene_tests`,
`outbound_queue_tests` and the live `worker_rpc_limits`/`worker_rpc_transport`.

**Next implementation task: 6.20c** — the production
author→preview→go→clear→restore→media-swap proof: one display-labelled
fixture driving a real `--socket` worker end to end, authoring through the
same `DocumentController` the panels bind and asserting each hand-off
through worker status and the preview stream, with environment limits
printed as honest skips in its own output.
This handoff does not mark 6.20 complete.

## Completed capabilities

| Checkpoint | Delivered | Remaining qualification |
|---|---|---|
| M0 | Plan, FreeShow reference, build/dependency foundation | Current dependency recipe is in CMake and `scripts/build-skia.sh`; packaging notices remain M7. |
| M1 | Separate worker, output/display selection, RPC, shared-memory previews, decode/upload/audio feasibility | Historical benchmarks and display evidence are archived; full projector/Wayland/X11 acceptance remains open. |
| M2 | Documents, service packages, history/recovery, compiled scenes | Later M6 fixes govern current durability, ownership and validation behavior. |
| M3 | Lyric authoring, typography, backgrounds, transitions, clear/restore | Later acknowledgement, fade and restore fixes supersede the original implementation notes. |
| M4 | Workspace, libraries, editor/style controls, shortcuts/settings | Real selected-scene previews and multi-item authoring were completed later in M6. |
| M5 | PlaybackPipeline, AvPlayer, video continuity, source controls and media errors | Production controls/standalone playback were completed later in M6; since 6.9b a prepared clip's bring-up is primed off the render loop. |
| M6 completed work | Production compositor, public Skia/ICU/build recipe, correlated RPC, honest presented state, preview/GPU ownership, audio/seek fixes, bounded transport/cache, durable assets/autosave, production media controls, worker editor/thumbnails, multi-item/fit/gradient authoring, SceneStore and the prepare executor | See the individual DONE rows in PLAN.md; 6.9's pair is closed and later completion does not waive M7 gates. |

## Architecture audit and optimization, 2026-10-29

[docs/architecture.md](docs/architecture.md) records current owners, source/test
entry points, protected contracts and differences from the intended design.
The original [M6 review](docs/M6-code-review.md) remains historical evidence;
it is not a list of findings that are all still unfixed.

The two-process split, Qt-free worker/domain, immutable scene boundary, bounded
metadata RPC, shared-memory previews, worker text layout and single-target
audience composition match the intended design. Important open differences:

- **7.2/7.4 (the residue of 6.9b):** a scene nobody primed still pays on the
  frame loop — an unprimed video opens there, `layoutFor()` shapes a layout it
  was not handed and `backdropFor()` builds a missing backdrop — and preview
  readback is synchronous, so deferring later attempts bounds average work, not
  one-frame latency. Prepared scenes are the measured path (the `6.9b` evidence
  below); the trio `status.scene.images` reports is what a miss still cost.
- **6.20:** production images and text share the words layer's ordered stack;
  the images toggle does not control authored foreground images. Independent
  layer controls need reconciliation with mixed item ordering.
- **6.20:** standalone and video use independent AvPlayers/PipeWire streams,
  differing from the plan's one software-mixed output stream. Explicitly accept
  that design or implement the shared sink before closing the review.
- **6.20:** background autosave is asynchronous; explicit save/load and
  decision/quit drains remain synchronous (**6.19b1** gave that coordination one
  owner, `DocumentPersistence`, and deliberately did not move the write off the
  UI thread). Missing foreground-image failures
  are logged but lack acknowledged per-item UI reporting.
- **6.18b, 6.19:** the ownership extractions are complete — the ledger has no
  components left to extract (6.18a2's `PreviewProducer`, 6.18b1's
  `BackgroundTransport` and 6.19a2's `PreviewConsumer` landed; 6.19a1's
  `RpcClient`, 6.19a3's `PresentationModel` and 6.19a4's `RpcMethods` landed
  2026-10-30, closing 6.19a; and all eight leaves of 6.19b landed the same
  day: `DocumentPersistence`, `DocumentViews`, `ShortcutMap`, `OutputPanel`,
  `PlaylistPanel`, `ShowPage`, `EditPage` and `SettingsPage`).
  **7.1–7.4:** recovery, resource/latency measurements, packaging and service/
  physical-display acceptance remain open. Pinned cache excess is reported,
  not a hard bound on total process memory.

The completed SceneStore path was optimized: it now copies each Scene once at
prepare, outside its lock, then shares that immutable instance with pins and
frame snapshots. This removes repeated deep copies and the separate capture
cache; the same revision can reuse the renderer's pointer-keyed layout cache
across audience and selected-preview drawing. Tests verify identity, isolation
from later caller edits, eviction survival and existing publication/thread rules.
No throughput or percentage speedup is claimed. The History lifetime comment was
also corrected: Documents must outlive the History that observes it.

PLAN.md now keeps concise deliverables/acceptance pointers for every completed
task. **All 122 task IDs and statuses are preserved**, as are unresolved task
acceptance and release gates. The previous full plan/progress files are preserved
under [docs/history](docs/history/README.md), including old commands, measurements,
decisions, mutation narratives, display caveats and superseded handoffs.

## Verification for this audit

- `CCACHE_DISABLE=1 cmake --build --preset headless -j 2`: PASS.
- `CCACHE_DISABLE=1 cmake --build --preset desktop -j 2`: PASS.
- `ctest --preset headless --output-on-failure`: **28/28 PASS**, 48.29 s.
  Vulkan used llvmpipe; live PipeWire subscenarios reported daemon-access skips.
- `ctest --preset desktop --output-on-failure`: **49/49 PASS**, 231.42 s,
  including all 12 display-labelled tests, Radeon 890M/RADV Vulkan and live
  PipeWire subscenarios. The first sandboxed run was interrupted after socket
  fixtures reported `Operation not permitted`; this full rerun used local access.
- Documentation ledger comparison: all task IDs/statuses unchanged; original
  snapshots preserved. `git diff --check`: PASS.

These checks do not establish projector placement/DPI/stacking, audible playback,
target-GPU timing, a thirty-minute A/V run or a one-hour mixed service soak.
Historical desktop/headless totals in archived logs are not fresh results.

## Working tree and exact next action

The working tree carries task **6.20b**. Tasks **6.20a** (`ebb282d`),
**6.19b8** (`88cb050`),
**6.19b7** (`f6752b0`),
**6.19b6** (`8722b9e`),
**6.19b5** (`cfc782c`),
**6.19b4** (`5446a6e`),
**6.19b3** (`337626e`),
**6.19b2** (`eaf5586`),
**6.19b1** (`7050a2d`),
**6.19a4** (`7cd5edf`, with the
6.19b split plan at `5aeeebc`),
**6.19a3** (`c00d025`),
**6.19a2** (`a4cb9e2`), **6.19a1** (`001588e`),
**6.18a2**, **6.18b1**, **6.18b2** and **6.9a** are committed at
`0907877`, **6.9b** at `d83469e`; what those moved is recorded below as well as
in their commit messages:

- `src/ui/SettingsPage.qml` (6.19b8) — the settings page as one component:
  the PRESENTER block (the number-keys toggle over
  `appSettings.numberKeysPresent`, the "presenter keys always work" line), the
  WINDOW block (the fullscreen toggle — the same persisted flag F11 flips and
  the window's `visibility` binds), the SHORTCUTS Repeater (fifteen rows,
  verbatim, each checked against the owner in `ShortcutMap.qml` before the
  move), and the SERVICE rows (`applicationVersion` with the worker's status,
  `docs.serviceDirectory`, the autosave-vs-Ctrl+S explanation). Its header and
  `ShortcutMap.qml`'s now point at each other: the map is where a key changes,
  the sheet is the promise the operator was given. 134 lines left `Main.qml`,
  which is 255 lines of composition: five page/dock instances anchored, the
  key map wired, the executors beside them.
- `src/ui/EditPage.qml` (6.19b7) — the edit page as one component: the
  `EditCanvas` child with the page's anchors reaching from its right/bottom
  edges inward (stage above the fields, style panel to its right), the
  `selectionAsk` timer behind `askForWorkerPicture()` (250 ms, restarted by
  every documents/text/navigation change while visible, payload compiled at
  the tick so the render shows how the slide ended up, `clearSelectionPreview`
  on leaving the page), the geometry `Row` whose `commit()` demands all four
  finite/positive values from `docs.setTextBoxGeometry` and whose `sync()`
  re-establishes the `editCanvas.boxX.toFixed(0)`-style bindings (objectName
  `geometryField` on all four), the STACK column's New/Delete/Back/Forward
  (enabled straight off `docs.currentBox` and the box-row count, refusal
  naming the limit on the status line), and the `stylePanel` — `targetId()`,
  `styleIdAt()`, the one-edit `commit(newAlign)` that forks a style on first
  edit and unwinds the creation with `docs.undo()` if the domain refuses the
  values, the `sync()` that maps style/asset/fit ids onto combo indices and
  leaves an unresolvable fit where it is, and every field and button under
  their objectNames. `boxSelected` is an alias on the canvas flag the key
  map's Escape pair hand off on; `nudgeBox()`/`stepBox()` moved because they
  read the canvas's live rectangle — the same one the drag commits, which is
  why one undo entry lands either way. 612 lines left `Main.qml`.
- `src/ui/ShowPage.qml` (6.19b6) — the show page as one component: the
  slide-actions row (`docs.addSlide`/`removeCurrentSlide`, the "slide n of m"
  counter), the 6.16b thumbnail band kept whole (`requestSlideBand()` with the
  ±5 window and the 32-cap reasoning, the `bandAskedFor` latch behind the one
  post-handshake ask, the three `docs` triggers and the `Component.onCompleted`
  first ask), the slide rows (worker `PreviewView` picture plus the legible
  line, the 46 px only when the picture shows, the highlight and click that
  selects and, when following, asks to present), the history row, the
  navigation row with Present arming follow, the `pictureInfo` column (both
  objectNames moved with it) and the lyric `TextArea` whose
  `onSlideTextChanged` re-establishes the `docs.currentSlideText` binding
  rather than assigning it. `following` moved from the window to the page —
  nothing outside the block ever read it, and with the follow checkbox local
  the Present/click/step logic reads its own state — so the page's only window
  contract is `presentRequested()` handled by the shared
  `presentCurrentSlide()`; placement, `visible: root.page === "show"` and the
  four dock/topbar anchors stay at the call site. 314 lines left `Main.qml`.
- `src/ui/PlaylistPanel.qml` (6.19b5) — the left dock as one component: the
  horizontal chip ListView over `docs.playlists` (current chip in --secondary,
  click to select, `+` chip styled as a chip because the Controls Button
  dropped its glyph at this size), the entry ListView over
  `docs.playlistEntries` with the section prefix, the resolved show name and
  the current-song highlight, the ▲/▼/✕ Repeater handles (disabled at the
  ends, each handle a one-step `movePlaylistEntry`), the row drag
  (`Drag.mimeData` `entry:<id>`, release-drops through the MouseArea), the
  DropArea whose nearest-row arithmetic — `Math.round((drop.y - y) / 30)` and
  the down-move `index - 1` — moved WITH the gesture rather than being
  re-derived, and the dirty-marked status line over `docs.serviceDirectory`.
  No properties, no signals: every command was the controller's, as the
  OutputPanel case was. 270 lines left `Main.qml`.
- `src/ui/OutputPanel.qml` + `PanelField.qml` + `PanelButton.qml` (6.19b4) —
  the right dock as one component: the `SidePanel` root reading
  `panels.rightWidth` and answering its own drag/click the way
  `LibraryDrawer` does, with the preview canvas (`PreviewView`, off-air
  placeholder and all), the stats line, the monitor picker bound to
  `worker.currentMonitor` so it shows where the output actually is until a
  switch confirms, the five layer checkboxes, the demo-scene row, the
  per-layer clears and the Restore toggle — each `enabled` straight off the
  acknowledged live flags, never a click — the transport column whose
  sliders re-apply status only while the thumb is at rest, the mixer rows
  across both transports, the standalone track row and the guarded
  device/frame/revision line. The dock reads no window state, so it takes no
  properties and emits no signals (the split's row guessed at follow-state
  and page; the real block never needed them); the window keeps the anchors
  and the `rightDock` id the centre pages use. The 4.4 inline components
  moved to module files verbatim — 367 lines left `Main.qml`.
- `src/ui/ShortcutMap.qml` (6.19b3) — the window's entire key table as one
  component: 49 `Shortcut` objects, each keeping the exact `sequence` and
  `enabled` expression it had in `Main.qml`, with the four composite gates
  (`nudging`, `boxStepping`, `presenterTyping`+`presenterNav`, `numberKeys`)
  composed inside the map from eleven fact properties (`page`,
  `typingIntoField`, `squattedFocus`, `drawerFocused`, `boxSelected`,
  `currentBox`, `slideCount`, `wordsOnAir`, `backgroundOnAir`, `canRestore`,
  `numberKeysPresent`) so who may answer a key reads next to what the key
  means. Actions come back as 20 intent signals handled in the window; the
  `Escape` restore-or-clear branch, the Ctrl+D slide-count split and the
  drawer's Ctrl+1/2 exemption are unchanged. The window keeps the executor
  functions (`nudgeBox`, `stepBox`, `presenterStep`, `presenterJump`,
  `presentCurrentSlide` — the last also serves the Present buttons), the two
  focus facts that only its `activeFocusItem` can answer, and F11's flip of
  the persisted `appSettings.fullscreen`. 240 lines left `Main.qml`; the
  registered-shortcut proof is the live notice set: the three multi-sequence
  `StandardKey` notices now name `ShortcutMap.qml:92/289/304` and nothing
  else changed.
- `src/ui/DocumentViews.{h,cpp}` (6.19b2) — the row shapes the operator's
  lists read, now one author inside `openshow-ui-core`: song rows, slide rows
  (first-line preview, first-box geometry, background asset and fit), the
  current slide's box rows, the style list and the resolved style with its
  gradient, playlists and entries with each referenced show resolved (or `?`
  and 0), the media registry, the drawer's search projection, and the selected
  box's text/kind/asset. The class is a stateless reader over a const
  `Documents` reference — nothing to fall behind, nothing to invalidate — and
  the navigation it needs arrives as a `Cursor` value the controller builds
  (`viewCursor()`), never stored. `selectedBox()` moved with the one fallback
  rule intact (named box by id, else bottom of the stack, else nothing), and
  the controller's private `selectedBox()`, every box-scoped edit and the
  canvas-facing rows all resolve through it, so R17's identity between the
  highlighted and edited box stays structural. `linesAsText` is the single
  `'\n'`-joined reading of a box's lines: `boxText()` uses it and
  `commitPendingText` compares the debounce draft against the same conversion.
  `findSlide` replaced the controller's file-local `findCurrentSlide` (its
  blur-on-video edit check and the style reads all call it); `textToLines`
  stayed with the edits. Every getter on the controller forwards; 261 lines
  left the `.cpp`.
- `src/ui/DocumentPersistence.{h,cpp}` (6.19b1) — the document session's
  durability, now one owner inside `openshow-ui-core`: the `AutosaveScheduler`
  as its child (so the executor's thread is retired with it), the task 2.3
  startup rule (`open()`: scratch wins over the saved service, and a recovered
  scratch is the reason the session starts dirty), the autosave edge (inline
  while dormant, submitted with the decision epoch once started), the deferred
  refusal surfacing with the `fAutosaveStale` race guard and the
  "current again" line, the decision protocol (`quiesce` raises the epoch and
  waits the executor out; `resume` reopens the path), the two decision points
  (`saveToService` writes and adopts and reports a survivor in the same words
  the controller used to write; `loadFromService` returns separate documents so
  the caller gives up what it had only on success), the quit-edge durable wait
  and the writes/coalesced counters. Its `Sink` is the host's status line and
  nothing else, so durability has an opinion the operator reads and no property
  of its own to bind. `DocumentController` keeps the documents, the history,
  navigation and the dirty flag: `save()` hands the rebinds to
  `History::adoptAssets` and emits `assetsAdopted` (R7), `load()` moves the
  documents in, clears history and re-anchors navigation, and `autosaveNow` is a
  one-line forward. 116 `.cpp` lines and 8 of header left the controller; every
  public persistence name and both survivor status texts are byte-identical.
- `src/worker/rpc/RpcMethods.{h,cpp}` (6.19a4) — the worker's method table and
  status/notification encoders, the fourth and last component of 6.19a.
  `MethodTable::dispatch(envelope, outbound)` answers everything the agreed
  protocol names after the connection's own gates: `ping`, `output.status`/
  `output.displays`/`output.select`, `layer.setEnabled`, `scene.prepare`/
  `scene.present`, the four `preview.*` methods, the six `media.*` commands and
  `shutdown` — each with the R11 rules intact (numbers read in their own width,
  ranges checked before any narrowing, the thumbnail batch capped before a
  single entry is parsed, refused keys clipped only on the way back out). The
  encoders (`statusResult`, `displaysResult`, `previewReadyParams`,
  `mediaErrorParams`) are pure functions over session snapshots, so the status
  lock is never held near the socket. The table holds no socket: `scene.prepare`
  hands its executor-thread ack to the `DeferredSink` its host injected, which
  is `RpcServer::submitDeferred` — the same mutex-guarded bag push and self-wake
  byte the inline lambda performed before the move. `RpcServer` keeps the
  envelope gate, the notification policy, the `hello` handshake, `run`/`serve`,
  `drainToSocket`, the deferred collection and `stop()`, and is the only thing
  that may end a connection; 576 lines where it had 1485.

- `src/ui/PresentationModel.{h,cpp}` (6.19a3) — the acknowledged output model's
  new owner, inside `openshow-ui-core`: the mirror of every prepare payload the
  session sent, keyed by the revision the store handed out and pruned to a
  32-revision window; the latest-go rule (`fLatestGoId`: a prepare ack presents
  only while its request is still the newest go, so a slow ack cannot regress
  the audience); the pending logical presentation (`fDesiredSceneJson`) that
  successive clears compose against instead of a stale confirmed status; the
  restore trio (`fRestoreJson` retained as CONTENT so store pruning cannot
  retire it, `fRestoreArmedGo` for the in-flight restore, `fRestoreDoneRevision`
  that must reach active before the snapshot retires); the `sceneContent`
  indicators derived from the ACTIVE revision's payload; and `adoptAssetPaths`,
  rewriting the mirror, the in-flight prepare intents (through the host's
  `eachPending` visit), the snapshot and the owed scene, re-preparing — never
  re-presenting — what the output is owed. Its `Outbound` bag is five callbacks
  the `WorkerController` injects: the live gate, the same `sceneIsSendable`
  budget a present and a selection pass (R11 keeps one owner), the request
  sender, `prepareHandle` (the `ScenePrepareAsk` created on the request itself
  after the send — 6.3a's one-record rule, `RpcRequest` stays out of the model),
  the visit over pending prepares, and the status line. It interprets only the
  `scene.prepare` ack, the `scene` block of `output.status` and a refused
  request's restore marker; `changed()` is forwarded as `sceneChanged`, so the
  six scene properties and every go/clear/restore/adopt invokable forward with a
  byte-identical QML surface. `WorkerController.cpp` lost 247 lines and its
  header 34; `fSceneJson`/`fMirrorMax`/`fSceneActive`/`fScenePending`/
  `fScenePrepared`/`fSceneContent`/`fRestoreJson`/`fRestoreArmedGo`/
  `fRestoreDoneRevision`/`fDesiredSceneJson`/`fLatestGoId` are no longer its
  members, and the three worker-reported revisions survive a lost connection
  exactly as they did before — re-confirmed by the next status poll.

- `src/ui/PreviewConsumer.{h,cpp}` + `src/ui/SceneAsk.h` (6.19a2) — the
  frontend's pictures' new owner, inside `openshow-ui-core`: the read-only
  mapping of the shm segment `preview.start` names (opened on the first
  `preview.ready` that proves it exists, retired when the worker says the stream
  stopped, when a new stream starts, and when the connection or the process
  goes), the audience mirror frame, the editor's off-air target and the slide
  list's band — each with its own generation counter, its asked/wanted dedupe
  and the frame gate that releases a slot rather than painting a superseded
  picture — plus the byte-bounded thumbnail LRU (count AND bytes, with a
  picture dropped when its slide's scene text changes) and the two status-poll
  stand-down rules that re-issue an ask the worker no longer holds. It reaches
  the worker through three injected callbacks — request, notification, and the
  same `sceneIsSendable` budget a present passes — so the 6.8a rule keeps one
  owner and the list, the canvas and the go button cannot disagree about what
  the worker will take; and it interprets no reply except its own methods'
  (`preview.start`, `preview.thumbnails`, the `preview.select` prepare ack) plus
  the `preview.ready` notification and the `preview` block of `output.status`.
  `ScenePrepareAsk` (in `SceneAsk.h`) is the intent record both preview asks
  decorate on a `scene.prepare` and the one the session's mirror write and go
  intent read: it left `WorkerController`'s private `Ask` so the two classes
  name the same record rather than two lookalikes — which is what keeps a late
  ack from answering the wrong feature. `WorkerController` keeps the process,
  the handshake and poll rhythm, the acknowledged output model and the QML
  surface; its preview getters forward, its `previewChanged` and
  `thumbnailReady` signals are the consumer's forwarded, and its scene-budget
  check is the predicate the consumer calls. `WorkerController.cpp` lost 565
  lines and `WorkerController.h` 118 more, with no behaviour changed.
- `src/ui/RpcClient.{h,cpp}` (6.19a1) — the frontend transport's new owner,
  inside `openshow-ui-core`: the `QLocalSocket` and its XDG_RUNTIME_DIR path,
  the shared `openshow::protocol` decoder, the connect-retry clock, the ledger
  of requests the worker owes (id → the method it was sent under, the connection
  generation it was written on, and the sender's own intent record), the three
  refusals (a frame over the transport's limit, 4 MiB of unread backlog, 64
  unanswered requests), and the 2 s deadline that ends a connection whose peer
  has taken not one byte — with the reason kept alive so the disconnect path and
  the process-exit path both report it. It reads no result: `WorkerController`
  answers `replyReceived` by the recorded method, which is R3's rule in one
  place. `WorkerController` lost ~180 lines and no behaviour, and the
  `PendingRequest` table it keyed by id is now an intent record the request
  carries home — so a decoration made after a send and the reply that finds it
  are the same record (6.19a2 moved that record out to `SceneAsk.h`, where the
  session and the preview consumer both name it).
- `src/worker/session/BackgroundTransport.{h,cpp}` — the media's new owner
  (`openshow-worker-session-transport`): background pipeline + audio feeder,
  the standalone track and its swap, the video seam's two bounded slots and
  their lease handoffs, the newest-wins intent slots, the test schedule, the
  typed faults and the whole media status mirror, behind the transport's own
  mutex (session -> transport, never the reverse; the R6 quiesce boundary
  stated in the header). `OutputSession` keeps the loop and delegates every
  RPC verb; `status()` copies the transport's mirror; the moved types carry
  `OutputSession::` aliases, so RpcServer compiles unchanged.
  `tests/background_transport_tests.cpp` (headless) pins it;
  `background_media_tests` untouched — its live gates are the acceptance that
  the extraction preserved behaviour.
- `src/worker/dev/main.cpp` (6.18b2) — the new `openshow-worker-dev`
  executable owns every development driver: `--render-test`/`--idle-check`,
  `--snapshot`/`--scene-file`, `--check-layers`/`--check-transition`/
  `--check-production`, `--pw-check`, `--av-bench` and `--list-monitors`. It
  compiles `dev/main.cpp` + `render/TestScene.cpp` with the same
  `session/OutputSession.cpp` and output sources as the product binary, so it
  drives the real thing. The product `src/worker/main.cpp` is socket mode only
  (~150 lines: `--socket`, `--monitor`, `--seconds`, `--vulkan-validation`,
  `--version`, `--check-deps`); the `--check-deps` report exists once in
  `src/worker/DependencyReport.h`, and `rpc/RpcServer.cpp` and the demo scene
  are no longer reachable from the shipped binary. The five CLI-mode gates
  (`render_scene_snapshot`, `render_layer_composition`,
  `render_scene_transition`, `render_production_compositor`,
  `render_idle_check`) now run the dev binary; `worker_check_deps` still
  proves the product binary, and `worker_dev_is_qt_free` guards the new one
  beside `worker_is_qt_free`.
- `src/worker/session/PrepareExecutor.{h,cpp}` (6.9a, review R10) — the
  bounded, cancellable owner of prepare-time file work
  (`openshow-worker-session-prepare`): one worker thread, at most
  `kMaxQueued = 4` accepted jobs including the running one, cooperative
  cancellation checked between images, EXACTLY-ONCE completion per accepted
  job (inline from `cancel()`/`stop()` for waiting work, from the worker
  thread otherwise), explicit rejections that hold nothing, and an
  idempotent `stop()` that flags the runner, retires the queue and joins —
  so readiness, error and cancellation are each a stated answer.
  `OutputSession::prepareScene(scene, jobId, onReady, revisionOut)` mints
  the store revision and pins synchronously, then schedules the preload;
  with no background media (benches) the completion fires inline.
  `RpcServer` defers the `scene.prepare` ack to that completion: the reply
  rides a mutexed deferred bag pushed by the executor thread and drained by
  the serve loop, woken through a self-pipe (`fWakeFds`) so a finished
  decode answers within milliseconds rather than at the mercy of the 50 ms
  poll tick — a tick's drift pushed a live `preview_selection_tests` ask
  behind the preview budget's multi-second deferral, and the wake pipe is
  the fix AND the product's readiness latency guarantee. Out-of-order
  replies are legal per 6.3a (ids correlate); refusal paths answer
  synchronously ("could not schedule" / "was retired"), and the frontend
  clears its selection-ask dedupe on a refused prepare, mirroring the
  thumbnail rule. `close()` stops the executor FIRST, and production main
  closes the session after `server.stop()` so final completions land in a
  live server. `status().scene.preparing` reports the executor's live count
  (leaf mutex read), and `SceneImageCache` gained a lifetime `preloadHits`
  counter surfaced in `status().scene.images` — with the ack deferred, the
  counter, not the wall clock, is what proves a re-prepare skipped the disk.
- PLAN.md: 6.18b split into **6.18b1** and **6.18b2**, both DONE; **6.9a**
  DONE (only 6.9b remains of the R10 scheduling pair); 5.1b's and 5.5/5.5c's
  cross-references updated. README.md documents the two binaries;
  docs/architecture.md intro/diagram, the RPC and demo-isolation boundary
  rows, a new prepare-executor contract row, and the 6.9a open-difference
  row (now closed) updated. No milestone tags.

Verification after 6.9a (all four tasks in the tree):

- `cmake --build --preset headless -j 2` and `--preset desktop -j 2`: PASS.
- `ctest --preset headless`: **32/32 PASS** — `prepare_executor_tests` joins
  the suite (fast completion, nonblocking submit, capacity refusal,
  waiting-job cancellation, stop() retirement, and a 200-round
  submit/cancel churn hammer asserting exactly-once and bounded `queued()`;
  ten further direct runs, no flake).
- `ctest --preset desktop`: **53/53 PASS**, 232 s, all 12 display-labelled
  tests. `background_media_tests` now carries the live 6.9a acceptance:
  four COLD 4K prepares fired unawaited fill the executor while
  status/displays/ping/preview.released probes are interleaved on the same
  socket; the single read loop matches every reply to its request id
  (out-of-order correlation, 6.3a), asserts probes answered while prepares
  were outstanding, `scene.preparing >= 1` in a status answered during the
  storm, worst probe RTT under 250 ms, `scene.preparing` draining to zero,
  and the deferred acks carrying real revisions. The cache-hit assertion is
  now the worker's own `preloadHits` counter (deterministic; the 4K
  testsrc JPEG decodes faster than the ack's poll-tick quantum, which is
  what made the old wall-clock comparison flaky).
- Flake ledger: one run in eight showed `the rpc-* worker answers shutdown`
  fail with the worker exiting cleanly on its own; the same failure
  reproduces on the pre-6.9a build (different scenario), and the absence of
  the "still owed" log proves the render loop ended before the command —
  the fullscreen test window occasionally closed by the compositor. It
  predates this tree; watch it, do not paper over it.

Verification after 6.9b:

- `cmake --build --preset headless -j 2` and `--preset desktop -j 2`: PASS.
- `ctest --preset headless`: **32/32 PASS**, 51 s.
- `ctest --preset desktop`: **53/53 PASS**, 245 s, all 12 display-labelled
  tests. `background_media_tests` carries the live 6.9b acceptance: a healthy
  clip is the audience's active output, a scripted 700 ms open is then fired
  as a `scene.prepare` whose ack is NOT waited for, and one read loop tracks
  both truths — the deferred ack by id and status probes by their own round
  trip. Measured: the prime acked in ~1.1 s off-loop while status answered in
  ≤0.3 ms and the audience's presented frames advanced 4 → 31 with a 50 ms
  worst no-frame gap; presenting the primed clip reached the air in ~15 ms
  (the open the audience would otherwise have waited on is already paid); a
  same-identity lyric change over the running clip landed in ~2 ms with the
  transport never restarted; 15 warm switches cost p95 ~9 ms and the
  draw-boundary trio stayed +0/+0/+0, which is the evidence separating
  "prewarmed" from "the first frame paid"; a blur backdrop came from the
  prepare; shutdown with the queue full answered in 0.1 ms.
- Flake ledger. `preview_selection_tests` failed three consecutive runs and
  passed at HEAD: the cause was the fixture, not the transport. Its armed
  worker pays the scripted 250 ms on the FIRST preview frame it draws — the
  audience's own mirror — and the producer buys that overrun back by standing
  the WHOLE preview down for 3× the cost (~750 ms, R13). The supersession race
  was landing inside a stand-down that belongs to nothing it tests: the first
  ask was never drawn at all (the loop sleeps to the end of the window), so
  nothing was superseded and `stale` stayed 0. What hid this was the loop being
  slow to build the stream after `preview.start`; 6.9b made it prompt. The
  fixture now waits for the mirror's overrun (`published`, `worstMs`) and for
  its repayment window to lapse before arming (`waitForPreviewQuiet`), which
  holds in either order. `scene_frame_tests` failed while the layout cache sat
  on the shared `SceneTextEngine` — the companion borrows that engine, so a
  6.16b isolation test caught the design error the day it was made; the cache
  belongs to each `SceneRenderer`.

Verification after 6.19a1:

- `cmake --build --preset headless -j 2` and `--preset desktop -j 2`: PASS, no
  warnings in either tree.
- `ctest --preset desktop`: **53/53 PASS**, 244.9 s, all 12 display-labelled
  tests. `worker_controller_tests` (47.1 s) is the acceptance for this
  extraction and was not touched: its scripted peer re-execs the test binary as
  the worker, and the fixtures that pin R3/R5/R11 still drive the real
  `WorkerController` through the new seam — S1's burst of three prepares the peer
  holds still mirrors each request's own payload and moves the audience only for
  the newest go, `never-answer` still trips the 64-request refusal ("requests
  are already waiting for the worker to reply"), `stop-reading` still trips the
  backlog refusal and then the 2 s "has not read a byte" abort carrying the depth
  the abort discarded, and the resuming peer still keeps its session.
  `ui_loads`/`document_present` prove the QML surface is unchanged.
- `ctest --preset headless`: **32/32 PASS**, 51.7 s (the preset builds no UI, so
  this gate only confirms the shared protocol library the transport uses).
- No new test was added and no assertion was edited: an extraction that changes
  behaviour is a failed gate, not a new expectation.

Verification after 6.20b:

- `cmake --build --preset headless -j 2` and `--preset desktop -j 2`: PASS
  (the desktop tree rebuilt against the changed `tests/CMakeLists.txt`; the
  headless tree needed no compilation — the guard is a script its ctest
  entry reads at run time, so the headless run below exercised the
  strengthened regex).
- `ctest --preset headless`: **32/32 PASS**, 51.4 s — including
  `skia_public_api_guard` with its new both-forms regex.
- `ctest --preset desktop`: **53/53 PASS**, 246.5 s, all 12 display-labelled
  tests (135.7 sec*proc).
- The guard pair run in isolation (0.28 s): positive green; negative exits 1
  naming **both** fixtures (`violation.cpp` quoted, `violation_angle.cpp`
  angle-bracket) — the strengthening is proven to have teeth, not just to
  pass. The manual invocation also counted the scan: 144 files under `src`
  and `tests` outside `fixtures/`.
- No behaviour test was edited; `protocol_tests`, `scene_tests`,
  `outbound_queue_tests`, `worker_rpc_limits` and `worker_rpc_transport`
  passed unchanged, which is the audit-table rewrite's acceptance — every
  number the table now prints is one those fixtures enforce.

Verification after 6.20a:

- `cmake --build --preset desktop -j 2` and `--preset headless -j 2`: both
  "no work to do" — the leaf changes `docs/` and the ledger only, so the
  green runs below are gates over exactly the code the matrix describes.
- `ctest --preset headless`: **32/32 PASS**, 51.5 s.
- `ctest --preset desktop`: **53/53 PASS**, 245.7 s, all 12 display-labelled
  tests (135.1 sec*proc).
- The preset memberships the matrix's H/D/D✦ marks claim were verified from
  `ctest --preset headless -N` (32 registered) and `ctest --preset desktop
  -N` (53 registered) plus `ctest -L display -N` (12), not from memory; the
  first extraction pass undercounted because ctest pads single-digit test
  numbers with an extra space, and the corrected inventories are what the
  matrix marks come from.
- No test, source or CMake file was touched; every claim in a matrix row
  names a fixture that exists in the stated inventory and passed this run,
  or states its open remainder (6.20b/6.20d, M7.1a–c, M7.2, or the honest
  read-by-hand note for the cheat sheet).

Verification after 6.19b8:

- `cmake --build --preset headless -j 2` ("no work to do" — no UI in that
  preset) and `--preset desktop -j 2`: PASS, no warnings.
- `ctest --preset desktop`: **53/53 PASS**, 245.1 s, all 12 display-labelled
  tests (135.0 sec*proc), first run. `ui_loads` (8.0 s) is the acceptance,
  untouched, and the live notice set was read by hand: still exactly five —
  the two `Flow` complaints unchanged at `OutputPanel.qml:120/210`, and the
  three multi-sequence `StandardKey` bindings now at `ShortcutMap.qml:96/293/308`,
  a shift of four lines from 92/289/304 caused solely by the cross-reference
  comment this leaf added to that file's header. `ui_links_qt` and
  `workspace_panels_tests` unchanged.
- `ctest --preset headless`: **32/32 PASS**, 51.7 s.
- No test was edited. The cheat sheet moved as literal text; its alignment
  with the key table was verified by reading both files, not by an automated
  test — the standing Qt-side limit (no automated keyboard test) that 6.20's
  record must carry honestly.

Verification after 6.19b7:

- `cmake --build --preset headless -j 2` ("no work to do" — no UI in that
  preset) and `--preset desktop -j 2`: PASS, no warnings.
- `ctest --preset desktop`: **53/53 PASS**, 246.2 s, all 12 display-labelled
  tests (135.7 sec*proc) — on the re-run. The first full run failed one test,
  `playback_queue_tests`, a worker-side media fixture with no QML involvement
  (it does not load the shell); it passed run alone (9.6 s) and passed the
  full re-run, and is recorded here as the flake it appears to be rather than
  absorbed. The acceptance is `ui_loads` (8.0 s) — the edit page instantiates
  at shell load even while hidden, so a broken component would have ended the
  run — and the live notice set read by hand beside it: the same five, none of
  them the page's. `document_controller_tests` (the box scenarios: selection
  following identity across delete/undo, `setTextBoxGeometry` as the one op
  both the drag and the fields commit, the refusal leaving the model untouched)
  and `scene_edit_tests` drive what this page calls, unchanged.
- `ctest --preset headless`: **32/32 PASS**, 51.5 s.
- No test was edited; every binding, objectName and gesture moved as literal
  text, and the key map's four canvas touchpoints are the only interface that
  changed — three of them from `editCanvas.*` to the same objects reached
  through `editPage`.

Verification after 6.19b6:

- `cmake --build --preset headless -j 2` ("no work to do" — no UI in that
  preset) and `--preset desktop -j 2`: PASS, no warnings.
- `ctest --preset desktop`: **53/53 PASS**, 246.3 s, all 12 display-labelled
  tests (136.1 sec*proc), first run. `ui_loads` (8.0 s) is the acceptance and
  was not touched; the live notice set was again read by hand — the same five,
  and the show page emits none of its own, which is what it printed from
  inside the window too (a page that failed to load would exit the shell early
  and fail the guard). `document_controller_tests` (3.5 s) still pins the
  seams this page calls: the debounced `setSlideText` round-trip against
  `currentSlideText`, and the `selectSlide`/`stepSlide` navigation the rows
  and Prev/Next drive; `document_present_tests` (0.9 s) covers the
  compile-and-present the `presentRequested()` handler performs.
- `ctest --preset headless`: **32/32 PASS**, 51.8 s.
- No test was edited; every binding — band arithmetic, latch, the
  binding-re-establishing `Connections` — moved as literal text, and
  `following` changed owner only in the sense that it moved with its last
  remaining reader and writer.

Verification after 6.19b5:

- `cmake --build --preset headless -j 2` ("no work to do" — no UI in that
  preset) and `--preset desktop -j 2`: PASS, no warnings.
- `ctest --preset desktop`: **53/53 PASS**, 244.9 s, all 12 display-labelled
  tests (134.8 sec*proc), first run. `ui_loads` (8.0 s) is the acceptance and
  was not touched; the live notice set was again read by hand — same five,
  and the left dock contributed none of its own, which is what it printed
  before the move too. `workspace_panels_tests` and `document_controller_tests`
  (the playlist scenarios: chips through `createPlaylist`/`selectPlaylist`,
  the entry rows through insert/move/remove with the dangling-show `?`/0
  row) drive the controller the panel renders, unchanged.
- `ctest --preset headless`: **32/32 PASS**, 51.5 s.
- No test was edited; every binding and the drop arithmetic moved as literal
  text.

Verification after 6.19b4:

- `cmake --build --preset headless -j 2` ("no work to do" — the preset builds
  no UI) and `--preset desktop -j 2`: PASS, no warnings.
- `ctest --preset desktop`: **53/53 PASS**, 245.8 s, all 12 display-labelled
  tests (135.3 sec*proc of them), first run. `ui_loads` (8.0 s) is the
  acceptance and was not touched; the live notice set was re-verified by
  hand beside it — the shell stays up and prints exactly the same five
  notices, with the two `Flow` anchor complaints now naming
  `OutputPanel.qml:120/210` where they live, which also proves the moved
  dock instantiated rather than silently failing to load. `ui_links_qt`,
  both `ui_loads_guard_*` negatives, `document_present_tests` and
  `workspace_panels_tests` are unchanged.
- `ctest --preset headless`: **32/32 PASS**, 51.6 s.
- No test was edited. The honest limit repeats from 6.19b3: nothing drives
  these widgets automatically (the display-labelled worker tests prove the
  commands they send, not the buttons), so the guarantee here is that every
  binding moved as literal text, and what a missed rename would do is fail
  `ui_loads` with an `is undefined` line — which is precisely the pattern
  the guard bans.

Verification after 6.19b3:

- `cmake --build --preset desktop -j 2`: PASS, no warnings (the headless
  preset builds no UI: `--preset headless` reported "no work to do").
- `ctest --preset desktop`: **53/53 PASS**, 246.9 s, all 12 display-labelled
  tests (136.5 sec*proc of them) — on the re-run. The first full run failed
  one test, `background_media_tests`, a 57 s display-side media fixture with
  no QML involvement (it does not load the shell); it passed run alone
  (57.1 s) and passed the full re-run, and is recorded here as the flake it
  appears to be rather than absorbed. The acceptance for this change is
  `ui_loads` (8.0 s), which stayed up with no load errors, and the live
  notice set verified by hand beside it: the shell prints exactly the
  notices it printed before — the three multi-sequence `StandardKey`
  bindings, now naming `ShortcutMap.qml` lines 92/289/304 instead of
  `Main.qml`, and the same two `Flow` anchor complaints — which proves the
  shortcuts registered from the new component rather than silently vanishing
  from a broken one (a `Shortcut` that failed to load would end the run:
  the type would not resolve and the guard would fail). `ui_links_qt`, both
  `ui_loads_guard_*` negatives and `document_present_tests` (1.0 s) are
  unchanged.
- `ctest --preset headless`: **32/32 PASS**, 51.8 s.
- No test was edited. The Qt-side honesty limit is worth stating: no
  automated test presses a key (`ui_loads` only proves the shell loads), so
  the gates' mutual exclusivity is guaranteed here by the gates being moved
  as literal text with their shortcuts, not by new coverage — closing that
  gap is a named M7 candidate, not a claim of this commit.

Verification after 6.19b2:

- `cmake --build --preset headless -j 2` and `--preset desktop -j 2`: PASS, no
  warnings in either tree (as with 6.19b1, the headless preset builds no UI,
  so the new component is compiled and exercised by the desktop preset alone).
- `ctest --preset desktop`: **53/53 PASS**, 245.2 s, all 12 display-labelled
  tests (134.8 sec*proc of them). Nothing was edited, because an unchanged
  test is the whole acceptance of an extraction: `document_controller_tests`
  (3.7 s) pins the rows through the controller's properties — the slide rows'
  first-box preview and geometry, `currentSlideBoxes` with kinds and asset
  ids, box selection following identity across delete and undo, the
  resolved-style map and its override flag, playlist entry rows with a
  dangling show reading `?`/0, and the debounce comparison against
  `currentSlideText` — while `library_search_tests` (0.01 s) pins the
  normalisation the moved `searchShows` projection still runs through,
  `workspace_panels_tests` and `scene_edit_tests` stay green,
  `document_present_tests` (0.9 s) shows the compile-and-present path still
  resolves the same current slide, and `ui_loads` (8.0 s) loads the real
  shell with every row-binding unchanged and exactly the five known notices.
- `ctest --preset headless`: **32/32 PASS**, 51.4 s.
- No new test was added and no assertion was edited, and no row gained, lost
  or renamed a key: the bodies came across verbatim, so the QML cannot tell
  which file built the map it is binding.

Verification after 6.19b1:

- `cmake --build --preset headless -j 2` and `--preset desktop -j 2`: PASS, no
  warnings in either tree (the headless preset builds no UI, so the new
  component is compiled and exercised by the desktop preset alone).
- `ctest --preset desktop`: **53/53 PASS**, 245.9 s, all 12 display-labelled
  tests (135.7 sec*proc of them). `document_controller_tests` (3.5 s) is the
  acceptance, and it was not touched: its durability fixtures drive the real
  `DocumentController` through the new seam — the recovery-over-service startup
  ("recovery brings back the unsaved slide", recovered work is dirty), the task
  6.12 adopt scenario where a recovered asset names a file inside the scratch
  tree and the save rebinds it, the inline tick's refusal landing DEFERRED and
  being suppressed when the scratch catches up before any event loop ran (with
  the "current again" line when it genuinely catches up), the `aboutToQuit`
  flush, the coalescing counts (`autosaveWrites` between 1 and 5 against
  `autosaveCoalesced` at 20 or more), the epoch veto ("the sleeping write never
  landed - a stale autosave cannot resurrect"), and both survivor status texts
  for a scratch a committed decision could not remove. `document_present_tests`
  (1.0 s) and `ui_loads` (8.0 s) prove the QML surface — every persistence
  invokable and the `serviceDirectory` property, which now reads through the
  adapter — is unchanged.
- `ctest --preset headless`: **32/32 PASS**, 51.7 s.
- No new test was added and no assertion was edited, and no file format
  changed: a package written before this change loads after it, because the
  domain calls are the same ones in the same order inside the adapter.

Verification after 6.19a4:

- `cmake --build --preset headless -j 2` and `--preset desktop -j 2`: PASS, no
  warnings in either tree; both link the same `openshow-worker` binary the
  live gates drive.
- `ctest --preset desktop`: **53/53 PASS**, 246.5 s, all 12 display-labelled
  tests (136.4 sec*proc of them). The RPC acceptance was not touched:
  `worker_rpc_lifecycle` (5.6 s) drives hello-gating, refusal and shutdown
  through the real binary; `worker_rpc_transport` (13.6 s) still pins the
  write-deadline behaviour — a trickle reader keeps its session, a peer that
  takes nothing is dropped with the backlog in the log; `worker_rpc_limits`
  (12.1 s) still gets the same argument refusals — every number in its own
  width, the thumbnail cap answered before any parse, unknown methods refused
  by name; `output_status_tests` (3.7 s) decodes the encoder's full field
  list. The preview and media suites (`preview_selection/ownership/budget/
  stream`, `background_media_tests`, `audio_mix_tests`) pass against the
  re-routed handlers unchanged — the `preview.ready` params and the media acks
  are byte-identical because the encoders moved verbatim.
- `ctest --preset headless`: **32/32 PASS**, 51.6 s.
- No new test was added and no assertion was edited, and no protocol message,
  error text or result shape changed: the frontend cannot tell which of the
  worker's two classes answered it.

Verification after 6.19a3:

- `cmake --build --preset headless -j 2` and `--preset desktop -j 2`: PASS, no
  warnings in either tree (the headless preset builds no UI, so the new
  component is compiled and exercised by the desktop preset alone).
- `ctest --preset desktop`: **53/53 PASS**, 244.0 s, all 12 display-labelled
  tests (133.8 sec*proc of them). `worker_controller_tests` (46.9 s) is again
  the acceptance, and again it was not touched: its go/clear/restore scenarios
  drive the real `WorkerController`, which now delegates every one of them —
  the prepare-burst mirror that cannot misattribute a payload, the latest-go
  rule against a slow older ack, the refused go that presents nothing, the
  composed successive clears, the restore that survives store pruning and the
  canRestore that goes quiet only on the confirming poll, and the asset-path
  adoption across mirror, in-flight prepare and owed scene. The live
  `document_present_tests` (0.9 s) and `preview_selection_tests` (15.4 s) pass
  against an unchanged worker — the selection ask still rides a `scene.prepare`
  whose intent the model now arms. `ui_loads` (8.0 s) proves the QML surface —
  all six scene properties and every go/clear/restore/adopt invokable — is
  unchanged.
- `ctest --preset headless`: **32/32 PASS**, 51.5 s.
- No new test was added and no assertion was edited, and no protocol message
  changed: the worker cannot tell which of the frontend's classes assembled a
  prepare or armed its intent.

Verification after 6.19a2:

- `cmake --build --preset headless -j 2` and `--preset desktop -j 2`: PASS, no
  warnings in either tree (the headless preset builds no UI, so the new component
  is compiled and exercised by the desktop preset alone).
- `ctest --preset desktop`: **53/53 PASS**, 246.0 s, all 12 display-labelled
  tests (135.9 sec*proc of them). `worker_controller_tests` (46.7 s) is again the
  acceptance, and again it was not touched: its preview and thumbnail scenarios
  drive the real `WorkerController`, which now delegates every one of them — the
  off-air ask that rides a `scene.prepare` and answers only the newest
  generation, the refused-prepare dedupe reset, the band that re-issues itself
  when the peer's status says it is not showing what it was asked for, and the
  byte-bounded cache. The live `preview_selection_tests` (15.5 s),
  `preview_ownership_tests` (3.7 s) and `preview_budget_tests` (3.7 s) pass
  against an unchanged worker, with `preview_stream_tests` still pinning the
  segment's own framing: producer and consumer still agree about stream
  identity, slot release and the budget's stand-down. `ui_loads` (8.0 s) and
  `document_present_tests` prove the QML surface — every preview property, both
  picture getters and `requestThumbnails` — is unchanged.
- `ctest --preset headless`: **32/32 PASS**, 51.6 s.
- No new test was added and no assertion was edited, and no protocol message
  changed: `SceneAsk.h` names the intent record the frontend already sent, and
  the worker cannot tell which of its two classes assembled a request.

Resume by inspecting Git status, then take the next open ledger line:
**6.20c**, the production author→preview→go→clear→restore→media-swap proof;
then **6.20d–e** — the three reconciliations and the recorded closure. They
are the last open lines of M6; do not claim M6 closure until all three
resolve and the remaining gates pass.
