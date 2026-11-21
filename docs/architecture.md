# Architecture and implementation audit

Audited 2026-10-29 against `1b54bdb` plus the SceneStore sharing change and the
6.18a2 `PreviewProducer` extraction described below. This is a source audit of
completed tasks, with current test results in
[PROGRESS.md](../PROGRESS.md). [PLAN.md](../PLAN.md) defines the intended design;
the [M6 review](M6-code-review.md) records the original findings, whose source
line numbers and descriptions are historical.

Shared-control remediation (2026-11-05): shared preview cards/collections and
inspector sections now have populated geometry and interaction regressions.
The [acceptance record](reviews/CS-remediation-acceptance-2026-11-05.md) documents
the actual-worker capture tour. Thumbnail consumers retain a deduplicated window
of at most 128 scenes, sending successive packets of at most 32, with a
128-entry/32-MiB cache (raised for fixed operator pictures in TC.12). Hidden Bible pages explicitly release Selection ownership;
returning reacquires it. RpcClient disconnects socket callbacks before its members
are destroyed. These changes require no document schema or wire protocol bump;
remaining TV/TA and physical M7 gates remain open.

Current assessment (2026-10-31): **6.23a–e are DONE**. The five async
integration findings from the [post-6.22 review](reviews/M6-post-6.22-review-2026-10-31.md)
now have focused regressions and full-suite verification, recorded in the
[closure evidence](reviews/M6-6.23-verification-2026-10-31.md). Retirement claims,
scene/media publication, standalone request generations, completed seek-clock
epochs and stage outcomes have explicit ownership contracts. M7 recovery,
resource budgets, packaging and physical/long-service acceptance remain open.

Earlier qualification (review of 2026-10-30 at code `8402e69`; closures
through 2026-10-31): the
[architecture follow-up review](reviews/M6-architecture-follow-up-2026-10-30.md)
finds seven remaining issues in shared shaping, GPU reclamation, audible retirement,
capacity handling, blocking media operations, standalone state and thumbnail
completion identity. Existing 6.21 DONE evidence stands; full architecture
conformance also required **6.22a–g** — and all seven have since closed, each
with its own fixtures, in the narrative below. The older closure narratives
were qualified by this source review, which did not run new tests; those
qualifications are now discharged leaf by leaf.

**6.22a** (shared shaping), **6.22b** (GPU reclamation), **6.22c** (audible
retirement) and **6.22d** (deferral under capacity) have since been closed: the
engine owns no shared
`FontCollection`, each threading context shapes in its own, and the two Skia
halves of the finding — unlocked family resolution, and text blobs completed
during a line's first paint — are recorded with fixtures. Device teardown
(`GpuYuvUploader::destroy`, `Swapchain::destroy`, `VulkanContext::destroy`) now
takes the same `graphicsQueueMutex()` a commit takes, so a retired transport's
`vkDeviceWaitIdle` is ordered against successor submissions; the review's
alternative (per-slot completion fences) is deferred to M7.1b, which owns
timeout and device-loss behaviour. And the retirement hand-off now establishes
the outgoing audio's silent state — `pause()` deactivates the old sink stream
and latches its clock before the lane exists — so a slow producer join extends
the player's object lifetime only, never its audibility. And the one-deep
retirement slot no longer blocks its realtime consumer: `applyBackground()`
reaches teardown only with the slot empty — otherwise it keeps the latest
wanted transition (`fPendingTransition`, latest wins, no queue) and returns
without a present, the status mirror keeps reporting the still-live transport
honestly, and the next `applyTransportOps()` pass retries after the reap.
The blocking-media finding (**6.22e**, split e1–e4 in the PLAN) is now closed
as well: the standalone duration is probed on the bring-up lane and travels
with the staged player, `media.standalone`'s accept probe runs on the prepare
executor behind a deferred reply, and a replaced or abandoned standalone
player is silenced at the hand-off and stopped on its
own one-deep lane (swaps behind it defer, latest wins), and the seek-completion
wait — up to 500 ms per feeder — now runs on its own flight: the pass fans the
jump out and a lane worker holds the wait, while the completing pass runs the
sink flush, the clock rebase and the fault report on the transport's thread;
momentary ops hold their turn behind an unfinished seek, and a transport
retired mid-flight takes its flight with it (the lane joins the seek worker
before the player's stop). No cold file parks the RPC thread, no audio join or
seek ack is paid on the frame tick. **6.22e (A5) is closed.** And the
standalone mirror now retires with its player: the swap's claim publishes the
pending-selection state — the wanted path with `live=false`, no position it
does not have, and the outgoing instance's error cleared for a replacement —
so status names the new selection as requested-not-yet-presented through a
slow bring-up instead of reporting the removed player as live beside a command
gate that already refuses it. **6.22f (A6) is closed.** And the thumbnail
completion now carries the destination it was read for: the lane records the
companion's cache generation and the decode extent at the hand-off, and the
collect installs only into a destination that still matches both — an extent
change or a stop/start during a delayed decode discards the stale picture and
re-decodes against the current one, while reuse across bands at one destination
stays valid. **6.22g (A7) is closed.** All seven follow-up findings are now
closed, each with the fixture named in its row below; the qualification this
section opened with is discharged, and full architecture conformance to the
[M6 review](M6-code-review.md) and this follow-up is claimed on that evidence.

## Ownership and dependencies

```mermaid
flowchart LR
    QML[Qt Quick operator UI] --> DC[DocumentController]
    DC --> D[Documents and History]
    DC --> DV[DocumentViews]
    DV --> D
    DC --> DP[DocumentPersistence]
    DP --> AS[AutosaveScheduler]
    DC --> SC[Scene compiler]
    SC --> WC[WorkerController]
    WC --> PM[PresentationModel]
    WC --> PC[PreviewConsumer]
    WC <-->|JSON-RPC control and scene metadata| RPC[Worker RpcServer]
    RPC --> MT[RpcMethods]
    MT --> OS[OutputSession]
    RPC --> OS[OutputSession]
    OS --> SS[SceneStore]
    OS --> PP[PreviewProducer]
    OS --> BT[BackgroundTransport]
    OS --> SR[SceneRenderer]
    OS --> AV[PlaybackPipeline and AvPlayer]
    OS --> GPU[GLFW / Vulkan / swapchain]
    SR --> GPU
    PP --> SHM[Shared-memory composed previews]
    SHM --> WC
```

These boxes are responsibilities, not all separate classes. Preview production
graduated into `PreviewProducer` (task 6.18a2) and the background media into
`BackgroundTransport` (task 6.18b1), both beside `SceneStore`, task 6.18b2 moved
the bench/diagnostic drivers into their own `openshow-worker-dev` executable,
task 6.19a1 moved the frontend's transport into `RpcClient`: the socket, the
framing, the reply ledger, the send bounds and the write-stall deadline, task
6.19a2 moved the frontend's pictures into `PreviewConsumer`: the shm mapping and
the mirror, off-air and thumbnail frames copied out of it, task 6.19a3 moved
the acknowledged output model into `PresentationModel`: the revision mirror, the
latest-go rule, the clear derivation and the restore snapshot, and task 6.19a4
split the worker side the same way — `RpcServer` is the connection (accept,
framed read, hello gate, bounded queue and deadline, drain, shutdown) and
`RpcMethods` is the meaning of a request (the method table and the
status/notification encoders). What shares `WorkerController` now is the process
supervisor and its QML facade, which is the division the plan intends; the
planned split 6.19b1–b8 — the `DocumentPersistence` and
`DocumentViews` adapters out of `DocumentController`, then `Main.qml`'s shortcut
map, output panel, playlist panel, show page, edit page and settings page —
has now fully landed. **6.19b1**: `DocumentPersistence`
(`src/ui/DocumentPersistence.{h,cpp}`, `openshow-ui-core`) owns the scratch, its
executor, the decision protocol around an explicit save or load and the wording
of a refusal or a survivor, and `DocumentController` keeps the documents, their
history and what a decision does to them. **6.19b2**: `DocumentViews` (same
target) shapes the rows the views read — songs, a song's slides and their
backgrounds, the current slide's boxes, styles and the resolved style,
playlists with each entry's show resolved, the media registry, the drawer's
search, and the selected box's text, kind and asset — and is a pure reader with
no state of its own: where a shape depends on where the operator is, the
navigation arrives as a `Cursor` value the controller builds from the ids it
owns, and `selectedBox()` is the single resolution of the selection with its
positional fallback that the controller's own box-scoped edits go through
too. **6.19b3**: `ShortcutMap` (`src/ui/ShortcutMap.qml`) holds the window's
entire key table — 49 `Shortcut`s whose gates the component composes itself
from fact properties passed in (page, focus holders, canvas selection, on-air
state, settings opt-in) and whose activations leave as intent signals the
window handles — because two enabled `Shortcut`s for one key make Qt drop the
key, so R17's one-live-owner-per-key rule is only checkable where every
claimant is on one page. **6.19b4**: `OutputPanel`
(`src/ui/OutputPanel.qml`) is the right dock — the preview and every output
tool below it — mirroring `worker` status and dragging through `panels` on
its own; the window keeps only the anchors and the id the centre pages use,
and the 4.4 inline panel primitives became shared module files so extracted
panels carry the look without borrowing a window declaration. **6.19b5**:
`PlaylistPanel` (`src/ui/PlaylistPanel.qml`) is the left dock — playlist
chips, the running order with its reorder handles, row drag and nearest-gap
drop target, and the dirty-marked status line — every gesture addressing the
controller directly, so the window again keeps only placement and the id.
**6.19b6**: `ShowPage` (`src/ui/ShowPage.qml`) is the centre's show page —
slide list with its worker-drawn band, history and navigation rows, picture
panel and lyric editor — and it owns follow mode outright (nothing outside
the block ever read it), so its one window contract is `presentRequested()`:
presenting is the only act the keyboard executors share. **6.19b7**:
`EditPage` (`src/ui/EditPage.qml`) is the centre's edit page — the canvas
stage, the debounced worker picture of the slide under edit, the geometry
fields, the stack commands and the style panel — and because only the canvas
tracks the live rectangle a nudge commits from, the key map's nudge and step
executors live on the page and its `boxSelected` alias, the flag the two
Escape owners hand off on. **6.19b8**: `SettingsPage`
(`src/ui/SettingsPage.qml`) is the settings page — the two persisted toggles,
the fifteen-row cheat sheet and the service rows — and it and `ShortcutMap`
now name each other: the map is where a key changes, the sheet is the promise
the operator was given. That closes 6.19b; `Main.qml` is composition —
placement, the page gate, the focus facts and the executors — at 255 lines.

The later Edit operator follow-up replaces the Library/service dock in Edit
with `EditorSlideRail`, a 220px virtualized list of only the selected
presentation's slides. Cards reuse the shared preview surface and request the
bounded worker thumbnail window; the canvas retains Selection. Clicks commit
pending words through the controller before selecting a slide, and rail arrow
navigation is excluded from canvas nudging. Hidden Edit releases its thumbnail
requests. Add slide lives in the rail footer; the old picker and Save/Load/
Undo/Redo/Remove toolbar actions are removed. Delete is an undoable slide edit
outside text fields, with keyboard undo and persistence/autosave retained.

| Boundary | Implementation and evidence | Assessment |
|---|---|---|
| Editable documents and history | `src/domain/{documents,history,storage}.cpp`; the Qt session adapter in `src/ui/DocumentController.cpp` with durability in `src/ui/DocumentPersistence.cpp` and the view rows in `src/ui/DocumentViews.cpp`; `domain_tests`, `document_controller_tests` | Matches. The frontend owns editable state and package writes; history has one observed Documents and bounded snapshots; since **6.19b1** exactly one class decides when either is written to disk, and since **6.19b2** exactly one place spells what a list row shows. |
| Immutable render scenes | `src/scene/{scene,demo}.cpp`, `scene_tests` | Matches. Compilation resolves styles, geometry, item order and asset paths; the worker validates scene JSON. The scene library currently depends on domain types/compiler code, but neither uses Qt or GPU APIs. |
| Process separation | `src/worker/CMakeLists.txt`, `worker_is_qt_free`, headless preset | Matches. GLFW/Vulkan/Skia/FFmpeg/PipeWire stay in the worker. Qt Quick, process supervision and document adapters stay in the frontend. |
| RPC | `src/protocol`, `src/worker/rpc/{RpcServer.cpp,RpcMethods.cpp,OutboundQueue.h}`, `src/ui/{RpcClient,WorkerController,PreviewConsumer,PresentationModel}.cpp`; protocol/limits/transport/controller tests; prepare-time file work in `session/PrepareExecutor.{h,cpp}` | Matches the bounded metadata transport. Since **6.19a1** the frontend half has its own class: `RpcClient` owns the socket, the framing, the ledger of requests the worker owes, the size/backlog/in-flight refusals and the 2 s no-byte deadline, and interprets no result — a reply returns with its request's method name, connection generation and the caller's intent record, which is R3's rule kept in one place (`WorkerController::onRpcReply` decides meaning). Since **6.19a3** the scene operations the facade sends are decided by `PresentationModel`, which holds no transport of its own and reaches the ledger only through the injected callbacks. Since **6.19a4** the worker half is split the same way: `RpcServer` is the connection (accept, framed read, hello gate, bounded queue and progress-measured write deadline, drain, deferred-answer bag, shutdown) and `RpcMethods` is the method table and the status/notification encoders, answering through an injected sink so the prepare executor never learns a socket exists; only the connection can end a session. The deadline's measurement was fixed in **6.20c**: it runs from the moment the queue starts OWING bytes, not from the last byte that moved, so a frontend that takes everything and then goes quiet is idle rather than gone, while a peer that truly stops reading keeps the queue continuously non-empty and is still dropped on the window. Worker socket writes are nonblocking; since 6.9a dispatch schedules still decodes and media probes onto the bounded prepare executor and answers each request from its completion (self-piped wake, ids correlate), so a cold file delays no other command. |
| Media transport | `src/worker/session/BackgroundTransport.{h,cpp}` over `playback/{PlaybackPipeline,AvPlayer}`; `background_transport_tests` plus the live `background_media_tests` | Matches since **6.18b1** (2026-10-29). The transport owns the background pipeline and its feeder, the standalone track and its swap, the video seam's two bounded slots and their leases, the newest-wins intent slots, the typed faults and the media status mirror; the render-thread-owns-the-transport rule is stated in its header. Since **6.9b** (2026-10-30) it also owns the one primed background: `primeVideo` opens a prepared clip's file half on the prepare executor and parks it as the single prime, the render thread attaches that prime's GPU half at the frame boundary (or opens cold), and a superseded prime is closed on the thread that made it. Since **6.21c1** the prime carries the clip's audio half too: `primeAudio` starts a HELD `AvPlayer` on the executor (`Config::startHeld` — connection and negotiation, feeders, mixer and pre-roll all paid there, nothing audible, clock parked) and `applyBackground` adopts it under `fMutex`, retiring the current audio instance through `retireAudioInstance()` — swap under the lock, stop outside it — so the frame boundary releases a hold instead of building a transport. Since **6.21c2** the DEATHS run on lanes too: a teardown requests the producer's stop, nulls the atomic presenter, and moves pipeline plus audio player into a one-deep retirement slot whose thread joins the producer and stops the player; the render thread closes the GPU half (`reapRetirement()`), strictly after the join, when the lane reports done. The presenter is atomic and `submitVideo` filters by producer generation, which is what lets a successor start before the old pacing thread returns — R6's ordering is kept (close follows join; the retired objects live until their thread is reaped), only its WAIT moved. A second teardown while the lane waits DEFERS (task 6.22d): the newest wanted transition lives in one `fPendingTransition` slot, the call returns without a present, and the next transport pass retries it after the reap. The standalone track swapped the same way: the swap claims the current instance under `fMutex` (fresh silent placeholder), stops it outside, stages the new clip held on a bring-up lane, and a later transport pass publishes it — one stage at a time, a newer intent stays pending; shutdown drains both lanes. Since **6.22e1** the bring-up's result carries the track's duration (probed once on the lane), so publish reads no file under `fMutex`; since **6.22e2** the selection's accept probe runs on the prepare executor (`validateStandaloneTrack` then `armStandaloneTrack` behind `OutputSession::requestStandaloneAudioDeferred`) and the answer travels as a deferred reply — the RPC thread no longer opens files; since **6.22e3** the replaced or abandoned instance dies on a second one-deep lane of its own (pause at the hand-off, `stop()`'s joins on the lane, a swap behind a busy lane defers latest-wins) — the render thread no longer joins audio threads at all; since **6.22e4** the transport's seek is a one-deep FLIGHT (`beginSeekFlight` fans the jump out on the pass, a lane worker holds the per-feeder wait, the completing pass runs the finish half — sink flush, clock rebase — and the fault report, later momentary ops hold their turn behind an unfinished seek, and a teardown carries the flight to the retirement lane, joining the seek worker before the player's stop) — so no seek acknowledgment is paid on the frame tick either; since **6.22f** the swap's claim publishes the pending-selection state for a replacement as well as a removal (wanted path, `live=false`, no position, outgoing error cleared), so the mirror, the command gate and ownership move in one step and a removed player is never reported as current. The session keeps the loop's wake gate and the frame's present/lease calls, delegates the RPC verbs, and reads the mirror in `status()`. |

| Requested versus presented state | `src/worker/session/SceneStore.{h,cpp}`, `scene_store_tests`, `output_status_tests` | Matches. Decide captures commands together; landed state is distinct from published state. OutputSession reports the store's published state after successful presentation. |
| Composition and text | `src/worker/render/{SceneRenderer,SceneText,Layer}.{h,cpp}`; scene/frame/Unicode/pixel tests | Matches the shared worker text renderer, public Skia/ICU integration and direct audience composition. Since **6.9b** the layout cache belongs to each `SceneRenderer` — a companion borrows the engine's fonts and shaping, never the audience's four slots, its LRU clock or its miss counter (R16) — and a prepare shapes into the production renderer's own cache. Since **6.21a** the cache's contract is retention: slots hold `shared_ptr<const SceneLayout>`, `find()`/`put()` hand out copies of that handle, and replacing or evicting a slot releases only the cache's own reference — a draw holds the bytes it paints until it returns, whatever a concurrent prepare stores. Since **6.20d** the two layer toggles are a kind filter over the one authored item stack — `words` and `images` each hide their own kind, omission never reordering (see the open-differences table). Since **6.22a** the engine builds no shared `FontCollection`: `fontCollection()` hands every threading context its own instance over the shared `SkFontMgr`, so a frame-loop miss and a prepare-executor prewarm no longer resolve cold families inside one unlocked table. Shaping ownership has a second half: `skparagraph` completes a line's text blobs during that line's first paint, so a layout handle crosses threads by hand-off (prewarm → frame loop) but is painted by exactly one thread at a time — a rule production keeps by ownership, one frame loop per renderer. |
| Video and audio ownership | `src/worker/{media,playback}`, `src/worker/render/YuvGpu.cpp`; GPU/decode/queue/control/mix/sync tests | Matches bounded queues, displayed-image leases, generation checks and independent audio controls. Since **6.9b** a prepared clip's demux/decode half opens on the prepare executor (`PlaybackPipeline::openFile`) and the render thread's part is the Ganesh attach (`attachGpu`); a clip nobody primed still opens wholly on the render loop. Since **6.22b** every whole-device wait is a guarded queue operation: `GpuYuvUploader::destroy()`, `Swapchain::destroy()` and `VulkanContext::destroy()` hold `graphicsQueueMutex()` around `vkDeviceWaitIdle`, which is what the installed registry's `implicitexternsyncparams` require of it, so a retired transport's reclamation (6.21c2's lane, or a shutdown reap on the command thread) can no longer overlap a successor's guarded submit. The wait itself remains whole-device; fence-based per-slot retirement belongs to M7.1b with timeout and device-loss. Since **6.22c** audible deactivation is separate from object destruction: the teardown hand-off calls `pause()` on the outgoing `AvPlayer` (server consumption stops inside one slice, the clock latches, no thread is joined) before the retirement lane exists, so the lane's `join()`-then-destroy sequence extends the retired player's lifetime only — never its audibility. Since **6.22d** the retirement slot's one-deep capacity is a policy, not a wait: `applyBackground()` defers the newest transition into `fPendingTransition` (latest wins) when the slot is busy and returns without owing a present, `applyTransportOps()` retries it on the pass whose reap emptied the slot, and the teardown's old sleep loop survives only as a logged invariant violation on routing bugs. |
| Previews | `src/worker/session/PreviewProducer.{h,cpp}` over `src/worker/preview/PreviewStream.cpp`; the consumer is `src/ui/PreviewConsumer.{h,cpp}` (task 6.19a2), read through `WorkerController`; `preview_producer_tests` plus ownership/selection/budget tests | Matches. The producer owns the stream lifecycle, target arbitration, generation gates, thumbnail queue and budget policy; the session hands in frames and wakes the loop. Since **6.21d** a thumbnail's file read runs on its own one-deep decode lane: the RPC pump collects a finished decode (install + settle by path) or hands the next path to the lane and returns without waiting, so a cold 4K backdrop holds no command hostage; one file in flight keeps the memory and lookahead story of the old one-per-tick rule, and shutdown joins the lane. Since **6.22g** a completion carries the destination it was read for — the companion's cache generation and the decode extent, captured together at the hand-off — and the collect installs only while that destination still stands, so an extent change, a stand-down or a stream restart during a delayed decode discards the stale picture and re-decodes for the current one; reuse across bands at an unchanged destination stays valid, and the status counters describe the standing companion only (no destination, no decoded bytes). The consumer owns the mapping, its own two ask generations, the asked/wanted dedupe, the byte-bounded thumbnail LRU and the stand-down re-issues, and asks only through the transport its host injects. Shared memory with exact stream/sequence release; output and audience mirror use the same captured frame; edit selection and thumbnails deliberately render separate scenes. Readback is synchronous. |
| Persistence | `src/domain/storage.cpp`, `src/ui/{AutosaveScheduler,DocumentPersistence}.cpp`; domain/controller tests | Matches durable staged package writes, asset adoption and coalesced background autosave. Since **6.19b1** the coordination has one owner: `DocumentPersistence` runs the decision protocol (quiesce, write or discard, remove the scratch the decision made stale with its survivor reported, reopen), the deferred refusal surfacing that keeps the crash net, the 2.3 startup rule and the quit-edge durable wait. Explicit save/load and decision waits remain synchronous. |
| Demo isolation and dependencies | `render/TestScene.cpp`, `cmake/OpenShowSkia.cmake`, `scripts/build-skia.sh`; production/pixel/public-API guards | Production starts black and uses SceneRenderer without a demo builder. Since 6.18b2 the demo scene and the diagnostic drivers compile only into `openshow-worker-dev`; the product binary cannot reach them. Skia is a pinned submodule with public application includes and compatible ICU. |

## Themes and the appearance chain (milestone TH)

Appearance is one entity, the Theme (the type the project called Style until
**TH.1**, which renamed it end to end and moved `service.json` to format 6:
the array `themes`, the slide assignment `themeId`, the show default
`defaultThemeId` and the session pointer `currentThemeId`). The reader still
loads formats 1–5 under their old key names and refuses a file that mixes the
two spellings rather than guessing which half to trust.

Every slide's drawn appearance resolves through one chain, per property, in
`Documents::appearanceForSlide`: the slide's own customization first, then the
session's current theme, then the slide's `themeId`, then the show's
`defaultThemeId`, then the embedded **Default** theme — since the 2026-11-02
amendment (**TH.9**) the built-in look exists as a real entity: reserved id
`default` (outside the `styleN` sequence, so migration can never collide),
seeded into every `Documents` at construction and re-ensured after every
successful service load, appended as the chain's last layer (deduped),
editable like any theme but refused by `removeTheme` and disabled in the
Theme Editor's Delete button, so the floor is always there and always
reachable. Customizations are sparse
(`SlideCustomization` — a property absent from the struct follows the chain;
TH.2), and the answered empties are real answers, not absences: a gradient
`""` says flat whatever the theme grades, a family `""` says whatever the
platform picks, a size `0` says the renderer's own. The compile (TH.3) still
emits fully resolved styles into the scene JSON, so the wire, the worker, the
preview streams and the acknowledged revision flow never learned the chain
exists — a theme or customization edit re-compiles affected scenes exactly
like any other document edit.

The operator surfaces (TH.4/TH.5) keep one rule above all: **a shared entity
is edited in the Theme Editor alone.** The More ⋯ header menu selects or
clears the current theme; the Theme Editor mode lists themes (New, Duplicate,
Rename, Delete with references cleared as one undo step) beside a live sample
preview rendered through the selected-scene lane — the sample is a fixture,
never a document, so browsing themes touches no output. The Edit page's
inspector shows the RESOLVED appearance and its fields write per-slide
customizations only, each customized field carrying a revert badge. (PT.7
later removed the Show header's separate default picker and the cue tile's
theme/customization menu: whole-item application moved to the left-hand rows,
and per-slide styling stays in Edit.) Refusals (a bad colour, a
restore with nothing under it, a dead id) edit nothing and land on the status
line. Two Qt 6.12 shapes are deliberate: the context menus are flat because a
nested submenu's Repeater delegates never instantiate (the plan's "Apply
theme ▸" would be a menu that cannot open), and the header selector is a
plain AbstractButton over a Menu because a Material ComboBox embeds a
writable text field the Show screen's visible-tree audit does not permit. A
focus-out is also not an edit: `editingFinished` fires when a field merely
loses focus, so committing the value the chain already resolves to restores
the binding instead of customizing the slide.

A theme also carries WHERE its words go (the operator amendment, **TH.7/
TH.8**): one optional `placement` rectangle in the 1920×1080 authored canvas,
stored inside format 6 as a member of a theme entry and of a slide's
`customizations` — absent means no opinion, so nothing about a pre-amendment
document moves on open (a pre-6 file carrying the key is refused rather than
guessed). Placement resolves through the same chain, and the compile stamps
the resolved rectangle onto the slide's FIRST TEXT item only: additional
boxes and every image item keep their authored positions, so a deliberately
stacked slide does not collapse under a theme. The routing rule the operator
sees, hardened by the 2026-11-02 amendment (**TH.9**): every drag or typed
geometry write on the slide's PRIMARY box records a placement CUSTOMIZATION
(one revert unit "placement") whether or not a theme currently speaks — for
that row the authored rectangle is read-only legacy, compiled only while the
whole chain stays silent, so a nudged slide never half-writes a themed look.
Any other box — an additional text item, every image — keeps editing authored
geometry exactly as before. Since BX.1–BX.3 the box is placed directly:
drag its body to move and any of the four corners to resize, on the Edit
canvas and the Theme Editor's sample with one shared `BoxTransform`
implementation. The numeric X/Y/W/H band, the Centre H/V helpers, the owner
sentence and the old placement action row are gone; `slideBoxes()` still
carries the APPLIED rectangle, so canvas chrome is drawn where the audience
sees the words even when no theme is in play. The restore-customizations
controls still remove placement with the rest of a slide's custom edits — one
restore, one undo step, as the operator ruled.
While the theme has no opinion the overlay shows the built-in lyric box
dashed: an offer, not a record. Placement reset — the theme's own placement on
Songs, the view's box/inset on Bible — and the Bible's Use-theme's-box borrow
live on the box's context menu, opened by right-click or Menu/Shift+F10 and
routed through the same adapter/domain verbs, so a reset is at most one undo
and preserving a sibling field is the adapter merge's existing rule.
Two Qt 6.12 shapes mark this surface too: `Rectangle` has no dashed border,
so the offer outline is a small `Canvas` stroke, and the inspector fields are
re-read through `rowFor` on every write-driven sync because a handler running
inside `documentsChanged` sees the `row` property's binding before it has
re-run — the cached row would replay the numbers the write just replaced.

Evidence: `domain_tests` (format-6 round-trip, migration, mix refusals, the
precedence matrix per property, and the placement matrix — silent theme,
chain walk, one revert unit, storage refusals), `scene_tests`/
`document_present_tests`/`frontend_journey_tests` (the chain through to
acknowledged pixels, and the themed rectangle driving the FIRST TEXT item
while stacked boxes and pictures keep authored geometry),
`document_controller_tests` (the TH.5 surface API and the TH.8 placement
routing against real fixtures), `workspace_ui_tests` (menus, badges,
bulk-undo-once, the themed-state checks, and the placement gestures on both
pages) and the deterministic captures at 1680/1380/1000 taken with a show
default, a worn slide theme, two live badges and a themed placement visible
on both the Edit canvas and the Theme Editor. TH.9 adds its own rows: the
floor's seeding, refusal and chain tail in `domain_tests`, the primary-box
write-to-customization routing and applied-rect rows in
`document_controller_tests`, and the Default-aware list/editor/floor checks
in `workspace_ui_tests` — the captures now lead the theme list with Default.
BX adds its own rows there: real-pointer press/move/release through all four
corners and both centre axes at 1000/1380/1680 on the Edit canvas and the
Themes overlay in Songs and Bible, handle reach at a slide edge, a small
rectangle's body move, the selected handle over an overlapping box,
identity-owned cancellation by navigation/selection/view/Escape, the box menus
by mouse and keyboard, and the absence of every numeric geometry control (with
no blank spacer left behind). `inspector_adapter_tests` proves the reset/borrow
verbs preserve sibling fields and survive save/reopen, and
`production_journey_tests` publishes a moved and narrowed box through the real
worker — preview-only, then an explicit Go whose audience pixels match the
off-air box — with the rectangle surviving save/reopen and the media identity,
layer count and authored words intact. The same binary re-runs the pointer
matrix at `QT_SCALE_FACTOR=1.5` and `2`, and the populated capture path is
generated at all three scales.
[themes-plan.md](ui/themes-plan.md)
carries the model and the acceptance rows; the PLAN ledger closes TH.1–TH.9.

### Text layout: where the words sit and whether they shout (TL)

Two more answers travel the SAME appearance chain (the text-layout group,
2026-11-16): `verticalAlign` (`"top"`/`"center"`/`"bottom"`) and `allCaps`
(a strict boolean). Both are sparse optional fields on a theme, a named text
context and a slide customization, resolved slide → context → nearest theme →
default field by field, so an explicit `allCaps: false` is an authored answer
rather than silence; the silent pair is centre / normal case, which is what
every earlier file drew. The service format is 14 over 13: a format-13 file
still loads with its pixels and IDs unchanged, and a pre-14 file naming either
key is refused by name and era rather than guessed. Copying a theme or writing
one field carries the whole entry (and one undo), and `removeTheme` treats the
floor as before. The scoped consistency review found one real bug: "Remove
customizations" decided whether anything was authored from a hand-kept list in
QML that omitted `fontUnderline`, so an underline-only slide could not take its
change back; the button now reads the domain's own sparse set
(`DocumentController::currentSlideCustomized`), which counts an explicit `false`
as the authored answer it is and needs no update when the vocabulary grows.

The pair is CONCRETE on the wire, exactly like the rest of the style block.
Scene format 8 carries both keys on every TEXT item (an image item carries
neither: a picture has no paragraph to anchor), and scene 3–7 payloads read
with centre/normal-case defaults, so older files and older workers keep their
pixels; a scene-7 payload that NAMES either key is refused by name. Protocol
minor is 20. `scene.text-layout.v1` names a worker that DRAWS the pair and
`text.measure.v3` one that MEASURES with it; both are withheld by a build that
does not yet draw them (TL.2) and earned in TL.3, so the frontend's
`sceneIsSendable`/`measureText` gates refuse a non-default answer toward a hello
without the name and say why, while centre/off content keeps the legacy payload
shape and reaches any worker.

The renderer owns both halves in ONE place (`SceneTextEngine`).
`verticalOffset` computes the paragraph origin from the free space
`max(0, boxHeight - textHeight)` divided 0 / half / all for top/centre/bottom;
`paragraphTop` reads it for paint, ink bounds and the fit verdict, so the three
cannot drift, and an overflowing fixed-size paragraph stays top-anchored,
preserving the old clamped centre. `displayLines` applies ICU's ROOT-locale
(`""`) uppercase exactly once, before SkParagraph shapes, wraps, fits or
measures — so language-specific casing never depends on the operator's machine,
authored line breaks and empty lines survive, and the source strings are never
rewritten (the words on the slide stay as authored; capitals are a display
transform). The transformed result is checked against the scene contract's
line/item/scene character budgets with a named refusal, and `scene.prepare` runs
that guard before the store grows rather than drawing truncated text or
allocating past the wire's limits. `text.measure` enters the same path: an ask
that named an anchor and supplied a box height gets POSITIONED bounds while the
intrinsic height and line count never move, and a v2-shaped ask keeps the
pre-TL anchor convention.

Text fitting also reserves space **inside** the authored box for outlines,
shadows and glyph overhang. `layoutInBox` is shared by measurement and painting;
it wraps at the available width and reports occupied height including those
insets. Bounds include the actual shaped glyph outlines as well as line metrics,
so tight line spacing cannot disguise visible letters outside the line box.
Import and reflow pass the actual box height through the worker adapter and
cache it with the complete measurement question. Reflow resolves the selected
snapshot through the target's named context (including a Bible box or inset),
so the box and typography measured are the ones the committed slides render.

Planning measures the type the audience will wear. `FlowStyle` and
`MeasureRequest` carry the pair, the packer forwards it, and both the
measurement cache key and `songPlanFingerprint` name it, so a toggle is a
different plan rather than a stale hit; `validateFlowStyle` refuses an off-list
anchor before any measurement. `DocumentController::fixtureBoxStyle` reads the
pair off the compiled `SceneItem` the added slide will wear, and both the song
and Bible importers put it on every measurement block. The import pages
regenerate on `documentsChanged`, so a Theme Editor/Edit toggle re-measures the
held words with no second network request; the planner clears its plan
immediately and the Bible page discards a plan whose inputs moved
mid-measurement, so a delayed plan can never enable Add. Committed slide
boundaries and labels are untouched.

The controls are shared, not copied (`TextStyleControls`): a Vertical row of
three in-repository paragraph marks immediately after the horizontal alignment
row, then one All caps checkbox, then line spacing — the same order on the Songs
theme, the Bible view and the Edit slide. Each icon has an accessible name and a
tooltip, takes focus by Tab and activates on Space/Return as well as a click,
and every row shows the resolved value, the own/inherited dot and the same reset
chip; a change is one inspector commit (one undo), the off-air preview re-asks
on `documentsChanged`, and no edit presents — the audience changes only on the
next explicit Go, including for the currently live cue.

Evidence: `domain_tests` (chain, explicit false, context precedence, clearing,
format-14 round-trip, byte-identical re-save, era/off-list refusals),
`document_controller_tests` (the theme row's `...Set` pair, whole-set and
one-field verbs with one undo, customize/revert, the context read model,
duplicate, `createThemeFromCurrent`, save/reload),
`scene_tests`/`protocol_tests` (scene 8 text-only keys, scene-7 reader and
refusal, the capability spellings, minor 20), `text_measure_tests` (v3
acceptance, malformed refusal, spelled centre/off measuring identically,
positioned bounds, the prepare-time budget refusal), `unicode_text_tests`
(ß→SS, ü→Ü, root-locale i→I, empty lines, source intact, budget refusals),
`scene_render_tests` (three distinct anchor origins, every pixel inside the
anchored ink rect, allCaps matching literal capitals),
`inspector_adapter_tests` (provenance/commit/reset on theme, view and slide),
`worker_controller_tests` (both capability gates open and closed, legacy-shaped
centre/off payloads), `control_set_ui_tests` (shared order, icon names, keyboard
activation), `workspace_ui_tests` (the underline-only and explicit-false Remove
customizations path), and `production_journey_tests`/`frontend_journey_tests`
(the preview-only edit → explicit Go → acknowledged pixels → reset/undo →
save/reopen journey with the source words and the same media asset preserved).
[text-layout-plan.md](ui/text-layout-plan.md) carries the brief.

## Online Bible import (milestone BI)

The importer brings ONE requested passage at a time from Bible Gateway into
today's schedule — for the operator, in five moves:

1. **More ▸ Import Bible Passage…** opens the page. It opens on its own
   notice — "Online Bible lookup — internet connection required" — and
   every failure keeps that line honest: an unreachable provider, a timeout
   or a page that no longer looks like a passage each answers with its own
   actionable sentence, and a Retry re-asks. Nothing else on the page has
   happened yet.
2. **Type one reference.** Start naming a book and the field finishes the
   sentence: a built-in table of the 66 books answers every keystroke as a
   dropdown — "1 cor", "1cor" and "i cor" all name 1 Corinthians — and
   Enter or a click completes the book while keeping the chapter and verses
   already typed. It is a helper, never a gate: text the table does not
   know shows no rows and still looks up exactly as typed, and the book
   list never touches the network. The translation picker opens on the
   session's default — NIV, New International Version — and leaving it
   alone is the same request as choosing it from the menu. Press Lookup:
   one page is fetched per click — the passage page for exactly that
   reference and version, and the importer refuses a page that answers a
   different translation. There is no Bible to download, no library to
   install and no background fetching, ever; the fetched words live in
   memory and die on a new lookup, a cancel or a close.
3. **Choose verses.** The answer arrives as a verse list, all ticked;
   untick what the reading skips. Changing the translation retires the
   verses rather than serving stale ones. The ticks and the words are pure
   importer state: nothing on this page writes to the schedule until Add.
4. **Read the generated slides beside the off-air picture.** The selection
   packs itself into slides through the theme the service wears right now —
   box, font, size and spacing measured by the audience's own text engine
   (protocol `text.measure`, so what the importer promises is what the
   output will draw). A long verse splits at sentence boundaries across
   continuation slides that still name their verse, the reference and the
   licence reserve measured room instead of crowding the words, and a box
   the words honestly cannot fill blocks Add with an explanation instead
   of shrinking anything. Clicking a slide row previews it OFF AIR — the
   audience keeps whatever it had. Wearing a different theme re-paginates
   from the words already held; no second network request.
5. **Add to today's schedule** is the page's only door out. The passage
   becomes one schedule-owned item — not a Library show — with its verses
   as slides, the verse labels in the slides' notes (the slide's words are
   exactly the passage), and the reference, translation and licence stored
   with it. One undo removes the whole item; removing it from the schedule
   leaves no copy anywhere. Closing the page after Add leaves nothing
   behind: there is deliberately no Save to Library, no Present button and
   no second destination anywhere on this surface.

After Add the item is ordinary schedule content: **it plays with no
internet at all** — playback reads only what the service file holds.
Further lookups need the connection again. Saving is the item's one
durable moment; the importer caches nothing anywhere else (the end-to-end
fixture walks the whole sandboxed home to prove no fetched bytes and no
deselected verse persist in any file).

The access approach, the provider's live markup facts and the delivered
shapes of each leaf are recorded in
[bible-import-plan.md](ui/bible-import-plan.md); the regression evidence is
`bible_import_tests` (parser), `bible_lookup_tests` (session),
`text_measure_tests` and `scripture_pagination_tests` (measured
pagination), `bible_import_page_tests` and the BI rows of
`workspace_ui_tests` (the page), and `bible_import_e2e_tests` (the whole
journey, offline playback and the disk witness).

## Text contexts and the shared text-flow engine (milestone TF)

The [2026-11-03 theme/editor review](reviews/theme-editor-2026-11-03.md) records
the limitations of this delivered model and schedules TV/TS/TR in PLAN.md.
Its richer theme views, workspace layout, effects and configurable transitions
are proposals, not behaviour implemented by the TF checkpoint described here.

A theme now knows that not all the text it wears is lyrics. A theme may
carry named **text contexts** — `themes[].contexts` entries holding
typography and geometry only: font, size, spacing, alignment, an optional
box and inset. Nothing else. Colours always come from the theme, and
nothing in the file format or any editor carries a line-count field,
because how many lines a slide carries is the measurement's answer, never
a number to dial in.

A slide may name a context (`text_context`). The resolver
(`appearanceForSlide`) layers that context between the slide's
customizations and the themes' values: the first theme in the worn chain
that carries the name supplies it, per property, and a name no theme
carries is no layer at all — every older file and every unnamed slide
resolves exactly as it did before contexts existed. The resolver also
reports WHICH context layered, and the pages that say where a passage was
set read that one word instead of re-walking the chain.

The Bible page names `scripture` on every lookup it runs: the measurement
style, the off-air preview and every added slide carry the name, so a
theme that has grown a reading-size context decides how its passages are
SET. The same chapter, two services differing only by the context,
measured on the real worker: the plain 96 px display look packs 19 slides;
a 32 px lectern context lands 4 — multiple verses per slide because that
is what the worker's measurement fits, with exactly the chapter's words
either way. A theme without the context leaves the plan sentence byte-for
byte as plain as it was. The Theme Editor's Scripture Reading section
edits the one entry — a toggle that creates it from the theme's current
look and box, fields for size, alignment, spacing and the reading box,
an empty field meaning silence — and states the lines the box measures out
as information only, through the same `text.measure` port the pagination
answers on.

The packing itself is one generic component, `openshow::importer::textflow`:
ordered blocks, a box, a typography, a reserved height, the measuring
port, and a caller-supplied labeler. Greedy whole-block packing, the
sentence-before-word split, continuation labelling and every honest
refusal live there once; `paginateScripture` is the scripture adapter,
and the future ASCII song import calls the same engine with stanzas. The
worker's SkParagraph remains the only measurer anywhere — nothing is
clipped, repeated or silently shrunk.

The design and per-leaf acceptance are recorded in
[text-flow-plan.md](ui/text-flow-plan.md); the regression evidence is
`domain_tests` (the context chain and the resolver's report),
`textflow_tests` (the engine on a scripted measurer),
`document_controller_tests` (the editor's context verbs: replace-or-add
merge, refusals, one-undo steps, the disk round-trip),
`bible_import_page_tests` (the 19-versus-4 A/B and the plan sentence),
the TF rows of `workspace_ui_tests` (the editor section), and the TF leg
of `bible_import_e2e_tests` (the previewed flow is the scheduled flow is
the flow the audience saw).

## Review closure: R1–R19 evidence matrix (task 6.20a)

The review's completion rule says every finding must have passing regression
evidence or an explicit M7 gate, and that "the new behavioural regressions
pass". This matrix is that accounting. A row is evidenced when its named
fixtures exist in the ctest inventory of the stated preset and pass its
recorded gate; the green run recorded for **6.20d** (54 desktop / 32 headless
entries at the time) is in
[`docs/history/progress-archive-2026-11-16.md`](history/progress-archive-2026-11-16.md),
and every fixture named below passed in it. Those counts are historical: the
current inventory is the one `ctest -N` reports, and the current green run is
recorded in [`PROGRESS.md`](../PROGRESS.md). Preset marks: **H** runs in the headless
inventory (and therefore in both), **D** is desktop-build only without a
display label, **D✦** is desktop-build and display-labelled. Where M6
deliberately left work, the row carries the explicit M7 reference from the
review's own mapping; where this matrix found a finding bare, it records it
bare. The matrix reports; it does not paper.

| Finding | Closed by | Fixtures pinning it | Open remainder |
|---|---|---|---|
| **R1** production output used the development scene | 6.1a–b; 6.18a1, 6.18b2 | `production_output_tests` (D✦: fresh `--socket` worker reads black, uniform including the footer region), `render_production_compositor` (H), `render_scene_snapshot`/`render_layer_composition`/`render_scene_transition` (H), `worker_is_qt_free`/`worker_check_deps` (H) with `openshow-worker-dev` carrying the demo/bench drivers | none |
| **R2** repeated audio starts and standalone-only playback failed | 6.7a–b; transport state in 6.18b1; audio-contract decision 6.20d | `audio_mix_tests` (H: restartable mixer, stable source identities, sinkless clock semantics), `av_sync_tests` (H: audio→audio, silent→audio, failure→retry), `background_media_tests` (D✦: live transport restarts) | none — the two-transports-vs-one-mixed-stream wording is resolved as the accepted design with reasoning (6.20d); both-streams-audible is **7.4** device acceptance |
| **R3** rapid scene requests could show the wrong slide | 6.3a–b; 6.19a1, 6.19a3; 6.23c | `worker_controller_tests` (D: ID-correlated replies, connection generations, latest-go-wins, backlog refusals — unchanged across both extractions), `scene_edit_tests` (D: clear-derivation against the pending logical state); `background_transport_tests` and `async_integration_tests` (request generations reject older probes after removal) | none for B3; standalone intent ordering is covered by 6.23c. |
| **R4** status acknowledged state before a frame presented it | 6.4; 6.18a1; 6.23b | `output_status_tests` (D✦: applied fields credit only the successfully presented frame; command injected during apply; acquire/present failure injected), `scene_store_tests` (H: desired/landed/published distinct); `background_transport_tests` and `async_integration_tests` (A→B→C/clear retains the current scene, media identity and preview pixels until the whole transaction lands) | none for B2; session-owned deferral and presentation-time publication are covered by 6.23b. |
| **R5** preview release did not prove ownership | 6.5; 6.18a2, 6.19a2 | `preview_stream_tests` (H: exact published-sequence acceptance, stale/future/odd rejected), `preview_producer_tests` (H), `preview_ownership_tests` (D✦: stop/start with outstanding frames, delayed releases, consumer-owned slot never changed) | none |
| **R6** a held video token outlived its ownership | 6.6a–b; 6.18b1; 6.21c2 (retirement re-expression); 6.22b (queue-guarded reclamation); 6.22c (silent hand-off); 6.22d (deferral under capacity); 6.23a | `background_media_tests` (D✦: paused/final frame redrawn while decoder attempts reuse; quiesce-before-replacement; slow-exit retirement keeps frames clean across the overlap), `background_transport_tests` (H; with a real clip's audio, a real PipeWire stream and a headless device: the retired player paused at the hand-off and its server delivery frozen across the scripted slow exit; A→B→C and A→B→clear deferred at 0.0 ms and landed on a loop pass while A's scripted retirement ran), `gpu_decode_tests`/`playback_queue_tests` (H: displayed-frame lease, generation check; reclamation waits behind the queue's guard, twelve overlapped reclaims beside a committing producer); `background_transport_tests` (a request arriving at a completed-stage claim cannot overwrite an occupied retirement lane) | none for B1; checked retirement reservation and preserved ownership are covered by 6.23a. Device-loss handling remains 7.1b. |
| **R7** saving recovered media deleted the active source paths | 6.12; 6.19b1; 6.21b; real-worker adoption 6.21g | `document_controller_tests` (D: recover→save→present→edit→undo/redo→save with the original source removed — the review's probe sequence as a regression), `domain_tests` (H: durable package-owned adoption), `worker_controller_tests` (D: a foreground image adopted settled and mid-prepare, its per-prepare item paths asserted through clear-all/restore), `frontend_journey_tests` (D✦: both plates renamed on disk, adopted without moving the audience, and the later restore draws both pictures from the new paths with the old names gone and no foreground fault) | Closed by **6.21b / F2**: adoption rewrites every path-bearing scene item, not just the background, across mirror, in-flight prepares, desired content and restore snapshot. Real-worker adoption coverage landed with **6.21g**. |
| **R8** save atomicity protected only the manifest | 6.13a–b | `domain_tests` (H: staged content-addressed assets, commit after durable bytes, fault-injected copy/commit against an existing package, ID validation and symlink-aware containment) | recorded scope: complete bytes guaranteed; strict version rollback not promised |
| **R9** private Skia dependencies and an ICU impersonation | 6.2a–c; guard re-read 6.20b | `skia_public_api_guard` + `skia_public_api_guard_negative` (H: `CheckNoSkiaPrivate` over the production sources, the negative proving the guard can fail), `unicode_text_tests` (H: combining marks, emoji sequences, CJK wrapping, mixed direction), `gpu_yuv_tests`/`gpu_decode_tests` (H: independently specified conversion expectations) | standing practice, not a gap: public APIs still require controlled upgrades, no ABI guarantee. The 6.20b re-read confirmed the directory-scoped glob covers every module added since 6.2a and closed one real gap: Skia's root is an exported include directory, so `<src/...>` resolved at compile time while the guard matched only quoted includes — it now refuses both forms, and the negative fixture trips on both |
| **R10** preparation blocked RPC and presentation | 6.9a–b; 6.18b2; 6.21c1–c2; 6.21d; 6.22d (deferral under capacity); 6.22e1 (staged standalone duration); 6.22e2 (deferred standalone probe); 6.22e3 (standalone stop on a lane); 6.22e4 (seek wait on a flight) | `prepare_executor_tests` (H: exactly-once, capacity, cancel, stop contract), `background_media_tests` (D✦: status/displays/release answered during four cold 4K prepares; slow-retirement present→air 4.5 ms and bed-swap ack 1.6 ms against 700 ms scripted waits, audience gap ≤ 26 ms; a 600 ms standalone probe deferred off the RPC thread with status RTTs ≤ 0.8 ms through the window), `preview_selection_tests` (D✦: status and release ≤ 0.6 ms through a 600 ms scripted cold thumbnail decode, shutdown mid-read drained), `background_transport_tests` (H: a scripted 500 ms standalone probe costs no transport pass and no status read; a scripted 700 ms player stop costs the swap pass 1.4 ms with the retiring delivery frozen at the hand-off and the removal deferred, not waited; a scripted 300 ms seek ack costs the seek pass 0.1 ms with a mid-flight pause held and applied behind the completion), `render_idle_check` (D✦), `worker_rpc_lifecycle` (D✦) | **6.22e (A5) closed.** Standalone status transition closed by **6.22f** (`background_transport_tests`: through a 700 ms scripted bring-up the block says `live=0`, the wanted path and no position, and the gain command refuses the same way; the removal-only control reports the gone player live at a frozen position). |
| **R11** control transport limits were incomplete | 6.8a–b; 6.19a1, 6.19a4; audit table updated 6.20b; deadline measurement fixed 6.20c | `protocol_tests`/`scene_tests` (H: encoded-size, per-method budgets, envelope validation), `outbound_queue_tests` (H: bounded writer), `worker_rpc_limits` (D✦), `worker_rpc_transport` (D✦: slow-peer disconnect, no lock held against rendering), `worker_controller_tests` (D: in-flight bound, 2 s no-byte deadline), `production_journey_tests` (D✦: a long quiet frontend survives — the deadline runs from the debt, not from the last write) | none — the review's audit table now states the enforced numbers (1 MiB both directions, depth 8, scene budgets, outbound queue, echo clip, thumbnail bounds) rather than review-time claims |
| **R12** decoded-image memory grew for the worker's lifetime | 6.10 | `scene_image_cache_tests` (H: byte-accounted eviction, budget-exceeding sequence, missing→repaired same-path recovery), `background_media_tests` (D✦: pins across live transitions) | pinned resources above the eviction budget are reported, not evicted — open-difference row above; measurable budgets are **M7.2** |
| **R13** preview mutated live transitions; fades were not a lerp | 6.11a–b | `scene_frame_tests` (H: one immutable audience frame), `preview_budget_tests` (D✦: previews cannot advance fades), `live_fade_tests` (D✦), `scene_render_tests` + `render_layer_guard`/`_negative` (H: image→image and same-image pixel composites, no intermediate targets) | synchronous GPU readback remains in the render loop, bounded by the deferred-after-overrun budget — open-difference row above; cost measurement is **M7.2** |
| **R14** recovery writes blocked the UI and errors were discarded | 6.14a–b; 6.19b1 | `document_controller_tests` (D: coalescing scheduler, decision epochs an older write cannot outrun, refusals surfaced with the crash net kept, close-during-debounce flush), `domain_tests` (H: reusable unchanged-asset copies) | explicit save/load and decision drains remain synchronous by recorded decision — open-difference row above |
| **R15** promised features existed only below the production boundary | 6.15a–b; 6.18b1 (production and bench drive the same object); audio-contract decision 6.20d | `playback_control_tests` (H: play/pause/seek/loop, per-source gain/mute as typed protocol), `background_media_tests` (D✦: standalone track lifetime independent of slide/video replacement; final frame and loop-off), `worker_rpc_lifecycle` (D✦: the verbs on the real socket), `ui_loads` (D) | none — the two production streams are the recorded contract (6.20d), their audible pairing on hardware is **7.4** acceptance |
| **R16** editor imagery and thumbnails did not follow the planned renderer | 6.16a–b; 6.19a2; 6.21a; 6.22a; 6.22g (destination-carrying completion) | `preview_selection_tests` (D✦: selected unpublished scene and bounded thumbnail batches over the shm transport, audience untouched), `worker_controller_tests` (D), `ui_loads` (D: band-drawn rows), `scene_render_tests` (H: retained layouts survive eviction, same-key replacement and a concurrent prepare churn; per-thread shaping collections, six-context cold resolution and a prepare→frame hand-off), `preview_producer_tests` (H: a 600 ms scripted decode outlived by a 64→256 extent swap and by a stop/start at the SAME extent — the cache is left holding only the destination that asked, byte-exact, with the stale picture discarded and re-decoded; the controls adopt it: wrong-extent pixels 16 KiB into a 256 KiB ask, and a second entry for a cache that was retired) | Fixed: retained layout lifetime (6.21a), per-thread shaping ownership (6.22a), completion compatibility (6.22g). |
| **R17** authoring was narrower than the plan | 6.17a1–c3; 6.19b2/b3/b7/b8; images/words toggle resolved 6.20d; gradient clear 6.21e | `document_controller_tests` (D: multi-box model, one selection resolution, geometry, ordering, images), `scene_edit_tests` (D: compile path; both gradient shapes cleared — the key leaves the payload, not just the value — and the flag counts a black-to-colour ramp), `worker_controller_tests` (D: S16 — the acked indicator lit for a gradient, the peer's per-prepare ledger showing the cleared payload naming no gradient, the restore naming it again), `scene_render_tests` (H: mixed text/image draw order, the words/images kind filter over the one stack, fits, gradient), `production_journey_tests` (D✦: the live audience sampled #F87C00 at the floor through a black-top ramp and #000000 at both ends after the operator's clear), `ui_loads` (D: canvas and inspector; the key table's one-owner-per-key rule verified live by its unchanged notice set) | Closed by **6.21e / F4**: `dropBackground()` erases the optional gradient (its absence is the flat answer the codec defines) and `sceneContent()` counts the key as a live background. Keyboard automation remains absent; kind-filter pixel coverage remains valid. |
| **R18** remaining failure handling was visibly incomplete | 6.7c (typed decoder/feeder/sink/background failure state); per-item foreground lane 6.20d; frontend projection 6.21f; 6.23e | `media_tests`/`av_sync_tests`/`playback_control_tests` (H: failures carried as typed state, not claimed-on-air), `background_transport_tests` (H), `output_status_tests` (D✦: S3 — a broken stack picture named per asset path in `output.status`, updating not duplicating across attempts, cleared by the repair without un-counting, media lane untouched), `worker_controller_tests` (D: S17 — the list projected into `foregroundErrors`/`foregroundFailureEvents` with path and reason verbatim, a second path extending it, the notice clearing on an acknowledged repair while the monotonic count stays counted and the transport's own fault line stays faulted throughout); `background_transport_tests` and `async_integration_tests` (superseded successful/failed stages emit no false error; current failures retain their own path and reason) | B5 closed by 6.23e. Recovery/device-loss remains **7.1a–c**, accounting **7.2**. |
| **R19** seek acknowledgements and frame deadlines disagreed | 6.7d; 6.23d | `playback_control_tests` (H: generation paired with the target actually applied), `av_sync_tests` (H: failed audio seeks not acknowledged), `playback_queue_tests` (H), `background_media_tests` (D✦: rapid, backward-after-loop and paused seeks through the real session with the shared deadline mapping); `background_transport_tests` and `async_integration_tests` (delayed backward/forward seeks keep presenting; paused scrubbing and resume retain the completed clock epoch) | B4 closed by 6.23d. Acoustic pairing and long-run drift remain **7.4**. |

The completion rule's running obligations sit with their own leaves: the
guard re-read and the audit table update landed in **6.20b** (the guard
grew teeth for the angle-bracket include form and its negative for both
forms; the review's table now carries the enforced numbers) and the
production author→preview→go→clear→restore→media-swap proof landed in
**6.20c** (`production_journey_tests`: the whole journey through the real
`DocumentController` and a real `--socket` worker, asserted in status and in
preview pixels; it also pinned the two revision counters — mirror frames
carry the present ack's output revision, `scene.active` and selection frames
the prepared revision — and its first green run required fixing the worker's
write deadline, which dropped an idle-but-healthy frontend by the first frame
owed after 2 s of quiet). **6.21g** added the night the review said was missing:
`frontend_journey_tests` runs the show through `WorkerController` and
`PresentationModel` themselves over a supervised real worker — the gradient
acknowledged, per-layer clears composed against the pixels, the restore
re-preparing across the store's 16-deep pruning (one id past the 18th bare
filler, so a remembered revision cannot be what returned), both renamed plates
adopted without moving the audience and drawn again from the new paths, and a
real missing-picture fault reaching `foregroundErrors` with its reason and
leaving it on the repair — so the frontend's own decisions, not a hand-written
socket stand-in, carry the journey evidence (`production_journey_tests` remains
the raw-protocol coverage). The three named architectural differences were
recorded as decisions in **6.20d**: the `words`/`images` toggles became a
kind filter over the one authored stack (code-to-contract, pinned by
`testWordsImagesFilterTheStack`), the two PipeWire streams became the
accepted audio contract with the reasoning and a **7.4** device gate
(contract-with-reasoning), and per-item foreground failures became an
`output.status` lane beside the media lane rather than inside it
(code-to-contract, pinned by `output_status_tests` S3). The closure itself —
final gates and this matrix's honest skips — was recorded in **6.20e** under
"M6 closure: the completion rule in writing" below.

## M6 closure: the completion rule in writing (task 6.20e)

The review's completion rule says M6 closes when "the new behavioural
regressions pass, the direct-private-Skia guard passes, API traffic is
measured, and the old gates remain green with skipped hardware checks
recorded honestly." Here is each clause against the run that proves it, and
then the checks no run here made — named at the point they were skipped, so
nothing has to be chased through scattered notes to find the edge of what
this milestone actually shows.

- **The new behavioural regressions pass.** Every R1–R19 finding above has
  its named fixtures, and every one of them sits in the stated preset's
  inventory and ran green in the final gate.
- **The direct-private-Skia guard passes.** `skia_public_api_guard` and its
  negative twin match both include spellings — quoted and angle-bracket,
  the gap 6.20b found and closed — and both run in the headless suite the
  final gate is.
- **API traffic is measured.** The audit table in
  [M6-code-review.md](M6-code-review.md) carries the enforced numbers
  rather than review-time claims: 1 MiB both directions, depth 8, the
  per-method scene budgets, the per-frontend outbound queue (1 MiB / 1024
  frames, nonblocking writes), the 160-byte echo clip, the thumbnail
  bounds; the stall deadline runs from when the queue starts owing bytes,
  not the last byte that moved (6.20c).
- **The old gates remain green.** Recorded for task 6.20d at `2fb3131`:
  desktop **54/54** (247.9 s, all 13 display-labelled tests, 137.8
  sec*proc), headless **32/32** (51.2 s), both trees building clean. Task
  6.20e changes prose only, so those runs describe the closing tree.

### Recorded skips

What this milestone's evidence did not attempt, each named at the point it
was skipped. Nothing in the matrix's rows or the four clauses above claims
these:

- **Audible playback.** The production journey (6.20c) runs a deliberately
  silent show — text and stills only; `pipewire-0` is present and unused —
  and the audio decision (6.20d) moved contract wording rather than code
  precisely because its open claim, two simultaneous streams audible on
  hardware at acceptable relative latency, is a device claim. Owed by
  **7.4**.
- **Target-GPU versus software-rasterizer distinction.** The journey's own
  output records that this machine's RADV device (AMD Radeon 890M) is not
  told apart from llvmpipe by any check the M6 suite has. Timing evidence
  on the target GPU is owed by **7.4**.
- **Projector placement, DPI and stacking; physical second displays on
  Wayland and X11; unplug/replug.** The display-labelled fixtures drive a
  real worker window (GLFW on Wayland here) and assert composition and
  hand-offs — never geometry on hardware. Owned by **7.1c** and **7.4**.
- **Long-run gates.** The thirty-minute A/V continuity, ten-minute deadline
  and one-hour mixed-service runs are not in the recorded 247.9 seconds;
  historical M5-era numbers stay historical, exactly as their ledger rows
  already say. Fresh runs are **7.4**. The ten-minute audience deadline gate
  has since been re-run on this machine's own device as PV.2's acceptance
  (2026-11-16, previews enabled at 30 fps): 15000 presents, 0 missed, drift
  max 16.3 ms — evidence the release gate can use, not a discharge of 7.4's
  other runs, audible pairing or physical checks.
- **Keyboard coverage.** The cheat-sheet/`ShortcutMap.qml` alignment was
  verified by reading both files against each other (6.19b3, re-checked by
  the unchanged live notice set in `ui_loads`); no automated keyboard test
  exists that could operate them. Recorded bare in the R17 row.
- **Restart and device-loss behaviour.** Worker supervision and restart
  reset, destination-removal disabling, device-loss classification and
  bounded acquire/fence waits are owned by **M7.1a–c** exactly as the
  review mapped them; no M6 fixture pretends to cover them.
- **Accounting and cost budgets.** Redraw/onPresented accounting, signed
  A/V drift, the pinned image cache's excess above its eviction budget and
  one synchronous GPU readback's worst-frame cost are counted where the
  counters exist, but no measured budget bounds them. Owned by **7.2**.

With the four clauses matched to the recorded runs and every skip named at
its point, the review's completion rule is discharged: **M6 is closed** for
what those runs prove, and everything they do not prove has a numbered
owner in M7.

## Open differences from the plan

These limit what historical DONE statuses prove. The review linked above adds
6.21 corrective work; the rows below retain the original decisions and evidence.

| Difference observed in source | Consequence | Follow-up |
|---|---|---|
| **Closed by 6.9b** (2026-10-30) for the prepared path: `BackgroundTransport::primeVideo` opens a prepared clip on the prepare executor, `SceneRenderer::prewarmLayouts` shapes into the production renderer's own layout cache, and `preloadScene()` decodes a blur background's backdrop — all before go, all off the frame loop. What remains is the miss path: an unprimed video still opens on the render thread, `SceneItemsLayer::layoutFor()` still shapes a layout it was never given, and `SceneImageCache::backdropFor()` still builds a missing backdrop synchronously. | A primed scene's frame boundary costs no file work (measured: present→air ~15 ms with a 700 ms scripted open, warm p95 ~9 ms); a scene no prepare primed can still miss a frame deadline, which is the fallback's honest cost rather than a hidden one — the trio `status.scene.images` reports (`drawDecodes`, `drawBackdrops`, `layoutBuilds`) counts exactly those. | **7.2/7.4** soak the miss path against real media; keep it measured, do not re-describe the first blur build as outside the frame loop. |
| **Closed by 6.20d** (2026-10-30), code-to-contract: the `words` and `images` bits now gate the authored item stack TOGETHER (`SceneItemsLayer::dispatchGate()` answers to both) and filter its entries by KIND — `words` off draws only the stack's pictures, `images` off only its paragraphs, each kind keeping its authored position and fade alpha. A stack omits; it never reorders, so the authored overlap of whatever remains is exactly what it was. `FrameContext.layerMask` carries the mask per R13's content rule; the development decoration's stand-in photo remains what it always was, the `kImages` layer's own content, standing down whenever a presentation is on air. | The operator's two toggles control their own kind and nothing else: images off leaves words standing, words off leaves pictures standing, and hiding a picture reveals the words beneath it at the pixels they always held — not at pixels a re-stacked layout would pick. Snapshot path mirrors the gate (`SnapshotOptions.images`). | **Closed by 6.20d**, pinned headless by `scene_render_tests::testWordsImagesFilterTheStack` (four frames: both up, images off, words off, both off — magenta/fill/white-pixel positions prove kind filtering without reordering). |
| `DocumentController::save()` waits for autosave quiescence and calls `saveServiceAndAdopt()` inline; load and quit also drain synchronously. | 6.14b proves background **autosave**, not that all persistence is off the UI thread. | **6.19b1** gave the coordination one owner (`DocumentPersistence::saveToService`/`loadFromService`: quiesce, write, scratch removal and its survivor wording, resume), which is the precondition for moving it and does not itself move it — the write still runs on the UI thread. **6.20** records the remaining responsiveness limit. |
| Preview rendering/readback runs on the render thread after output; its budget defers future attempts after an overrun (now inside `PreviewProducer::fillPreviewSlot`). | It bounds average preview work and drops owned slots, but cannot prevent one expensive readback delaying the next audience frame. | Extraction done (**6.18a2**, 2026-10-29) without changing the policy. **6.9b** (2026-10-30) measured the loop THROUGH a slow off-loop open instead of assuming it: the audience's presented frames advanced with a 50 ms worst gap while a scripted 700 ms open ran on the executor. **7.2/7.4** still owe one readback's worst-frame impact. |
| SceneImageCache retains pinned resources even above its 192 MiB eviction budget. | The budget bounds evictable decoded data, not total process/GPU memory; pinned excess is reported. | **7.2/7.4** resource accounting and service soak. Do not describe it as a hard total-memory cap. |
| **Closed by 6.20d** (2026-10-30), code-to-contract: `preloadScene()`'s per-item failures now ride a dedicated reporting lane — `status.scene.foregroundErrors` names each asset path whose last prepare attempt failed, with that attempt's reason clipped to the echo budget, capped at 8 entries beside a monotonic `foregroundFailureEvents` that keeps truncation visible; entries clear when a later prepare's `refreshFor` decodes the path again. The acknowledged media lane stays background-only BY decision — a stack picture's decode error must not evict the transport's truth from `media.error` — and this lane sits beside it rather than inside it. | A missing foreground box is honest on screen (its box shows what is under it) AND now honest to anything asking: "which picture is missing, and why" is an `output.status` question — and since **6.21f** the operator's own panel asks it. | **Closed by 6.20d**, pinned live by `output_status_tests` S3 (missing file → named path + reason + event; re-prepare of the same broken path → updated entry, counted again, never duplicated; file put back → cleared, events not un-counted; `media.error` empty throughout). Frontend half closed by **6.21f**, pinned by `worker_controller_tests` S17. |
| The 6.18a2 extraction landed 2026-10-29: `PreviewProducer` is built (`openshow-worker-preview-producer`), used by OutputSession through a thin delegating seam, and pinned headless by `preview_producer_tests`. The 6.18b1 extraction landed the same day: `BackgroundTransport` (`openshow-worker-session-transport`) owns the background media and its status mirror. The 6.18b2 split landed the same day: the bench/diagnostic drivers and `render/TestScene.cpp` moved out of worker main into the `openshow-worker-dev` executable, leaving the product binary socket-mode only. The 6.19a1 extraction landed 2026-10-30: `RpcClient` (`src/ui/RpcClient.{h,cpp}`, in `openshow-ui-core`) owns the frontend socket, framing, reply ledger, send bounds and write-stall deadline, driven by an unchanged `worker_controller_tests`. The 6.19a2 extraction landed the same day: `PreviewConsumer` (`src/ui/PreviewConsumer.{h,cpp}`, same target) owns the frontend's shm mapping and the mirror/off-air/thumbnail frames, asking through injected callbacks and reading only its own replies, with `scene.prepare`'s intent shared through `src/ui/SceneAsk.h`. The 6.19a3 extraction landed the same day: `PresentationModel` (`src/ui/PresentationModel.{h,cpp}`, same target) owns the frontend's acknowledged output model — the revision mirror, the latest-go rule, the pending logical presentation the clears compose against, the restore snapshot and the asset-path adoption — with the facade's six scene properties and its go/clear/restore/adopt invokables forwarding unchanged. The 6.19a4 split landed the same day, closing 6.19a: `RpcMethods` (`src/worker/rpc/RpcMethods.{h,cpp}`, compiled into the worker binary) owns the method table and the status/notification encoders, and `RpcServer` keeps only the connection — and `WorkerController` keeps the supervisor and the QML facade, which is the planned division. The 6.19b1 extraction landed the same day: `DocumentPersistence` (`src/ui/DocumentPersistence.{h,cpp}`, `openshow-ui-core`) owns the scratch, its executor, the decision protocol, the startup rule and the durability wording, with `DocumentController` still the one documents-and-history owner and its public persistence surface unchanged. And the 6.19b2 extraction landed the same day: `DocumentViews` (`src/ui/DocumentViews.{h,cpp}`, same target) owns the row shapes the views read, a stateless reader over the documents with the navigation passed in as a `Cursor`, `selectedBox()` the single selection resolution the controller's edits also resolve through, and the controller's property getters forwarding. And the 6.19b3 extraction landed the same day: `ShortcutMap` (`src/ui/ShortcutMap.qml`) is the window's entire key table — every sequence, its gate, and its intent as a signal — with no key's owner, sequence or gate changed and the live notice set proving the shortcuts register from the new file. And the 6.19b4 extraction landed the same day: `OutputPanel` (`src/ui/OutputPanel.qml`, with `PanelField.qml`/`PanelButton.qml` becoming shared module files) is the whole right dock — preview canvas, monitor picker, layer toggles, per-layer clears and Restore, transport and mixer rows, standalone track and the device/frame/revision line — with the window keeping only placement. And the 6.19b5 extraction landed the same day: `PlaylistPanel` (`src/ui/PlaylistPanel.qml`) is the whole left dock — chips, running order, reorder handles, drag and nearest-gap drop target, the dirty-marked status line — its gestures addressing `docs` and its rows the `DocumentViews` projections. And the 6.19b6 extraction landed the same day: `ShowPage` (`src/ui/ShowPage.qml`) is the centre's show page — band-fed slide list, follow-presenting, history and navigation rows, picture panel and lyric editor — owning `following` outright and asking the window only to present. And the 6.19b7 extraction landed the same day: `EditPage` (`src/ui/EditPage.qml`) is the whole edit page — canvas stage, debounced slide picture, geometry fields, stack commands and style editor — with the nudge/step executors beside the canvas whose rectangle they move and one `boxSelected` alias for the key map. And the 6.19b8 extraction landed the same day, closing 6.19b: `SettingsPage` (`src/ui/SettingsPage.qml`) is the whole settings page — the two persisted toggles, the cheat sheet that mirrors the key table the operator was promised, and the service rows — and `Main.qml` is a 255-line composition of these components, placement, the focus facts and the executors. | Every planned component now has its own file and its one owner; nothing in the ledger remains to extract, and what this difference still owes is 6.20's review closure, not a boundary. | **Closed by 6.19b8** (2026-10-30). |
| Stable display identity, destination-removal disabling and complete restart/device recovery are not complete. | Historical display tests and successful launch/shutdown do not establish M7 failure recovery. | **7.1a–c**, physical Wayland/X11 and projector acceptance. |

## Contracts to retain during optimization

- `SceneStore` owns prepared IDs and desired/landed/published output state.
  Prepared scenes are immutable shared instances; a pin survives store eviction.
  The session acquires its mutex before the store mutex, never the reverse.
- `SceneFrame` captures one audience moment. Drawing another target does not
  advance a transition or publish a revision. Publication means successful
  submission/presentation to the window system, not proof of physical scanout.
- The audience composition pass creates no per-layer/intermediate render
  target or `saveLayer`. Preview/snapshot targets and cached CPU blur data are
  separate resources, not extra audience composition layers. Source guards
  supplement pixel tests; they are not a timing proof.
- RPC has a 1 MiB encoded JSON payload limit in both directions, a bounded
  outbound queue and 64 frontend in-flight requests. Original assets, decoded
  media and audio buffers never travel in RPC. Composed preview pixels use
  shared memory; ready/release messages carry metadata.
- Since **6.19a1** the frontend half of those bounds is `RpcClient`, and the
  rule it enforces is the one R3 was about: a reply is delivered to the request
  its id names, on the connection generation that request was written on, and
  with the intent that request's sender attached — nothing in the frontend
  dispatches on result shape, so a burst of prepares cannot misattribute a
  payload or fire a stale auto-present. A message that would exceed the payload
  limit, the 4 MiB backlog or the 64-request ledger is refused locally, with the
  reason and without consuming an id; the connection is ended only when the peer
  has taken not one byte for 2 s, which is a measurement of progress and never
  of queue depth, so a worker that is merely slow keeps its session (R11).
- Since **6.19a2** the frontend's pictures are `PreviewConsumer`, and the rule it
  owns is the one R16 was about: a frame is painted only when it belongs to the
  stream this client mapped and answers the newest ask that target made, so a
  late edit frame or a scrolled-past thumbnail is released like any other slot
  and never reaches a canvas. What the UI *wants* is kept separate from what the
  socket *carried*, which is why a cold start, a stream restart and a worker that
  stood a target down on its own each end with the picture rather than a
  placeholder; the thumbnail cache is bounded by count and by decoded bytes. It
  sends through the transport its host injects and applies the same scene budget
  a present passes, so authoring-side refusal stays one rule (R11), and it
  interprets no reply except the acks its own asks expect (R5).
- Since **TC** (2026-11-20) a service keeps its finished card pictures beside
  the shows: an optional, independently versioned `thumbnailCache` block in
  `service.json` (service format stays 15; an older build ignores the key), and
  a bounded `thumbnailVariants` block in `catalog.json` (format 3) for looks a
  Library song has already been prepared under. A record is the rendered slide
  surface only — Base64 PNG, checksum, declared size, render fingerprint — never
  card numbering, captions or operator chrome. Identity is a canonical hash of
  the compiled scene with paths and service ids emptied, each referenced
  asset's size and 8 KiB content probe, the renderer build/font-environment
  identity and the thumbnail policy, so a moved package and a save-adopt hit while replaced bytes, words,
  theme, reflow and font/renderer eras miss. `ThumbnailStore` decodes and
  encodes on two bounded background lanes, validates checksum/IHDR/size before
  a picture can be shown, keeps the record list under 256 records / 32 KiB per
  PNG / 1.5 MiB and answers `PreviewConsumer` before it asks the worker: an
  all-hit window sends no `preview.thumbnails` RPC at all, a mixed window asks
  only for its misses, and a worker render is recorded after its slot is
  released. Records never enter `Documents` or history; saves and autosave
  snapshots carry them, a clean writable service may be flushed cache-only
  under a canonical authored-digest gate (an external edit wins), and a cache
  failure can never fail an authored save. A prepared Library slide copies a
  matching variant under its new ids; the id-free identity is what lets a
  re-preparation reuse pixels while a different theme or lyric misses.
  Since TC.12, Show/Edit use one 320x180 operator picture and smoothly scale it
  for every card size. Smaller legacy records are retired at adoption and
  removed through the next cache flush/save; the Library bridge refuses to
  reintroduce them. Larger existing records still downscale within the codec's
  512x288 envelope. The resident 128-entry/32-MiB bound accommodates a complete
  fixed-size window without evicting its first batch as the last arrives.
  Memory validity includes the fingerprint; batch identity and encode serials
  retire stale results. Each image lane has two active jobs, at most 128 pending
  slides, and at most 8 MiB of queued encode images. Library writes coalesce
  onto one background snapshot, serialize with authored saves and join at quit;
  eviction keeps the six newest per-item variants.
  Evidence: `thumbnail_cache_tests`, `thumbnail_store_tests`,
  `worker_controller_tests` (persisted band), `document_controller_tests`
  (save/reopen/faults), `catalog_tests`, `song_service_tests` (look crossing
  datasets) and the measured `thumbnail_reopen_tests` journey.
- Since **6.19a3** the acknowledged output model is `PresentationModel`, and the
  truth it holds is the one R3 and R5 were about: a QML scene property reads a
  single owner, so an indicator cannot light from a request that was refused and
  a clear cannot compose against a payload the operator has already replaced.
  The revision mirror records only payloads this session sent, the newest go is
  tracked separately from the store's counters so a late ack for a superseded
  go cannot move the audience backwards, and a restore keeps its
  content until a status answer confirms the restored revision is actually on
  air. It sends nothing itself — its host injects the live gate, the request
  sender, the prepare-intent handle, the visit over pending prepares and the
  status line — and it reads only the prepare ack and the `scene` block of
  `output.status`, never the shape of any other result (R5).
- Since the DR corrections (2026-11-16) the Media Bin's file work obeys the
  same rule as prepare: `media.describe` and `media.thumbnail` run on their own
  bounded, cancellable `PrepareExecutor` (`fMediaWork`, separate from
  `fPrepares` so a bin thumbnail can never queue in front of a Go) and answer
  by deferred reply. The decoded frame is a worker-produced PNG in a bounded
  cache (`$TMPDIR/openshow-media-thumbnails`, 64 files / 32 MB / 24 h, the name
  fingerprinting the source's path, size, mtime and box), and the reply carries
  its path — pixels never ride the control connection. The frontend installs a
  reply only for the request it made for that path, so a reply that outlived
  its connection cannot repaint a card.
- Since **6.19a4** the worker answers requests through `RpcMethods`, and the
  division keeps the two rules the connection and the table were each trusted
  with: only `RpcServer` can end a session — a method is refused, never
  punished, and an unknown name is answered rather than dropped — while the
  table owns every argument width, range check and refusal text before a
  session operation is called, so an argument nobody sent still cannot move the
  audience at the new seam (R11). Operation names, error codes and result
  shapes remain the shared typed definitions in `openshow/protocol`: the split
  could not add vocabulary for the frontend and server to drift over (R3). The
  deferred `scene.prepare` ack crosses the seam as a shaped frame through an
  injected sink, so the prepare executor thread stays as socket-ignorant as the
  table that calls it (R10).
- Since **6.19b1** durability is `DocumentPersistence`, and the rules it keeps
  are the ones R7 and R14 were about. A decision owns the scratch alone: the
  epoch rises first, queued autosaves are dropped and an in-flight write waited
  out, so an older job can neither land after the clear nor ack as though it
  still mattered. A refusal leaves the crash net in place and lands DEFERRED so
  it is the last word the operator reads, with the stale flag suppressing a
  warning the scratch has already made untrue. A committed save or load that
  cannot remove the scratch it superseded NAMES the survivor, because a later
  startup would recover it as unsaved work. And because the scratch a save
  deletes may be exactly where a history snapshot points, the rebinds come back
  from the write for `History::adoptAssets` — the state that matters is the one
  `save()` returns with, in which no snapshot names a deleted path (R7). The
  adapter holds no documents and no history of its own; the controller stays the
  one owner and is the only caller.
- Since **6.19b2** the row shapes are `DocumentViews`, and the rules it keeps
  are the ones R16 and R17 were about. A row has ONE author: the running order,
  the drawer, the slide list, the canvas chrome and the inspector all read the
  same map, so the same fact cannot be spelled five subtly different ways on
  the way to the screen. The reader is stateless — it holds a const reference,
  caches nothing, emits nothing — so a row can never be one edit behind the
  model that owns it. And the selection resolves in ONE place: `selectedBox()`
  is the sole resolution of the cursor's box with its fallback to the bottom of
  the stack, the controller's box-scoped edits go through that same call, and
  the R17 promise that the highlighted box is the edited box is a property of
  the code. Navigation reaches the views only as a `Cursor` value the
  controller builds from the ids it owns — views cannot hold a stale position
  because they hold no position at all.
- Since **6.19b3** the key table is `ShortcutMap.qml`, and the rule it keeps is
  R17's: exactly one live `Shortcut` per key per context, because Qt drops a
  key two enabled owners claim rather than picking one. Every claimant on
  every key therefore reads in one file — Escape hands off between output
  clear and canvas deselect on the selection alone, Ctrl+D splits on slide
  count, the presenter arrows stand down whenever a nudging box wants them —
  and the gates moved WITH their sequences, never left behind. The map takes
  facts (which item holds focus, which page is up, what is on air) as
  properties and returns intents as signals: it never touches the worker,
  the documents or the canvas itself, so "what a key means" and "who may
  answer it" are one file and "what happens" stays beside the buttons that do
  it. Since **6.19b8** its operator-facing mirror — the cheat sheet — lives on
  `SettingsPage.qml` with both files naming the duty: a key changed in the map
  is a promise changed in the sheet, and the two are read side by side.
- Since **6.19b4–b8** the window composes rather than contains: the docks are
  `OutputPanel.qml` and `PlaylistPanel.qml` (SidePanel roots that read `worker`
  and `panels` and answer their own drags), the centre pages are
  `ShowPage.qml`, `EditPage.qml` and `SettingsPage.qml`, and the split's guess
  at window-to-panel wiring (five rows' worth of properties and signals) met
  the real blocks five times and produced one `following` that moved with its
  only consumer, one `presentRequested()` for the presenting the keyboard
  shares, one `boxSelected` alias for the Escape hand-off, and two executors
  that could not live apart from the canvas rectangle they move. The rule the
  shape encodes: a panel binds the root-context adapters directly, and the
  window keeps exactly what only it can speak for — placement, which page is
  up, `activeFocusItem`'s facts, and the executors.
- Prepare-time file work (task 6.9a) runs on `PrepareExecutor`: one worker
  thread, at most `kMaxQueued` accepted jobs including the running one,
  cooperative cancellation checked between images, completion fired exactly
  once per accepted job, and `stop()` retiring everything before anything
  the work touches is torn down. The store revision is synchronous truth;
  the `scene.prepare` ack is deferred to the completion and may pass later
  replies — ids correlate (6.3a). The session takes the executor's mutex
  only leaf-side (`queued()`); the executor takes none of the session's.
- A prepared scene's primed work is paid on that executor and consumed only at
  the frame boundary (task 6.9b). `PlaybackPipeline` splits in two: `openFile`
  (probe, format, decoders — any thread, no Ganesh) and `attachGpu` (the frame
  uploader and its ring — render thread only, like every other surface).
  `BackgroundTransport` holds AT MOST ONE primed transport, keyed by path
  under its own leaf mutex; `applyBackground` adopts it when the background's
  identity matches and pays a cold open when it does not. Whoever retires a
  prime closes it on the thread that made it, never on the render loop, and
  `shutdown()` retires the prime before anything it touches. Text layout and
  blur backdrop follow the same rule with their own owners: the layout cache
  belongs to the `SceneRenderer` whose draw reads it — never to the shared
  engine, so a companion cannot evict the audience's layouts (R16) — and the
  prepare path is the only prewarmer of either.
- The live preview transport uses three slots, normally 640×360 at up to
  30 fps when content changes; never overwrite an unreleased slot. The
  frontend asks for that cadence (task PV.1) and repaints on every accepted
  copy of any target, so the panel's refresh is the stream's, not the status
  poll's. Selected
  scenes and thumbnail batches have their own bounded requests and identity.
  `PreviewProducer` owns that stream and its targets (task 6.18a2): frames in,
  slots out — it composes, captures and credits no frame of its own. The
  session takes the producer's mutex after its own, the producer never wakes
  GLFW, and its `pin`/wake calls belong to the caller.
- Native audio decoding feeds bounded rings; the mixer feeds one PipeWire
  stream per AvPlayer. Background-video and standalone transports have
  independent lifetimes, so production owns two AvPlayers/streams. This is the
  **accepted design** (task 6.20d, closing the plan's one-software-mixed-stream
  wording with reasoning): the operator addresses two independent audio
  identities, and independence is the feature — the standalone track keeps
  playing through background swaps, each lane carries its own clock and its own
  fault domain (the 6.7c typed faults: a lane's `MediaFault::kAudio` recovery
  clears its own class and asset and cannot hide another lane's trouble), and
  PipeWire itself is the system mixer the two streams meet in.
  A shared output mixer was rejected because merging the streams re-couples the
  lifecycles that tasks 6.7 and 6.15a deliberately separated, and it would move
  the mix into our process where PipeWire already does it. What the decision
  still owes is evidence on hardware — both streams audible at once and their
  relative latency acceptable — which is **7.4** acceptance, not a code gate.
  PipeWire callbacks must not perform file/RPC/preview work.

## Optimization made during this audit

SceneStore previously stored Scene values, copied one at each `pin()`, and
maintained a separate copy/cache for frame capture. It now copies once on
prepare, outside its lock, and stores `shared_ptr<const Scene>`. Pins, iteration
and frame snapshots use that same instance. This removes repeated deep copies
and the capture cache, and allows selected previews of the same prepared
revision to hit the renderer's pointer-keyed layout cache. Separate prepared
revisions remain separate instances even when their values are equal.

`scene_store_tests` checks shared identity across repeated pins and capture,
isolation from later edits to the caller's Scene, and survival after eviction.
Existing publication, cap, transition and concurrent-command checks remain.
No performance percentage is claimed without a benchmark.

## Shared song Library and clipboard import (milestone SL/SI)

The Library is application data outside every service package:
`<app-data>/catalog/catalog.json` via `openshow::catalog`. Records carry a
stable global `itemN` id, a closed category (`songs`/`presentations`), a title,
independent `recordRevision`/`contentRevision` counters, canonical lyric
stanzas and the reporting fields `ccli`, `authors`, `copyright`. The catalog's
own `History` uses whole-state snapshots and a monotonic id watermark; the
undo stack need not survive a restart but every undo/redo result is written
durably before it is published. A strict reader refuses a future format,
unknown keys, unsafe/duplicate ids and unknown categories without touching the
target. `CatalogController` applies each edit to a copy, commits the file, and
only then publishes rows and history; an unreadable catalog reads empty and
refuses mutations rather than overwriting itself.

**Service rows reference the catalog.** Adding a song creates or reuses ONE
service show with `catalogItemId` and copies the attribution into the service
(service format 10; `Show::catalogItemId` and `Show::song`). Repeated additions
are separate running-order entries over that same show. `CatalogController::
itemChanged` renames and re-lays every linked show in one service undo step;
`itemRemoved` clears the link but keeps the attribution and slides, so a saved
service remains playable and reportable with the Library absent. A refused
compound service edit restores its pre-edit snapshot (domain `History`), so no
partial show survives. Library edits use catalog history; service edits use
document history; both stay separate.

**A show can carry one audio layer.** `Show::audioAssetId` (service format 13;
optional `"audioAssetId"`) names the media library track the show plays beside
its slides. The reference lives on the SHOW, so every slide of a song chart
compiles the same `<scene 7 "audio">` block and the worker's transport identity
keeps the track rolling while the words step. Dropping an audio card on the
running order creates a one-slide audio cue with no picture; dropping it on an
existing cue sets that show's layer. The worker mixes the layer and the
background video's own embedded audio as two inputs of ONE `AvPlayer`, so both
are paced by the same clock and reach PipeWire as one stream; an audio-only cue
starts the player with no playback pipeline. `media.mute` source `"all"` is the
master media-audio switch over every source (`output.status.media.muted`);
`media.gain` deliberately has no `"all"`.

**Media audio: one contract, and when it restarts** (DR.0, review D-series
2026-11-15). Three audio lifetimes exist and this section is their single
recorded policy:

| Lifetime | Owner | Starts / stops | Reported as |
|---|---|---|---|
| Cue audio — the show's layer, mixed with the background video's embedded stream | the CUE (the transport bundle the scene names) | with the cue's media identity; the player is one `AvPlayer` with up to two inputs | `output.status.media` (`path`, `sources[]` "video"/"audio", `muted`) |
| Embedded video audio | the same player, one input | with the picture that carries it | `sources[]` "video" |
| Production track (`media.standalone`) | the OPERATOR, independent of every scene | explicit `media.standalone`, and nothing else | `output.status.media.standalone` |

The cue workflow is the current one and is retained: a cue's audio is authored
on the show, not on a transport panel, and the rail has one master mute. The
standalone RPC stays as it is — a separate player with its own lifetime, kept
for the API's history — and no scene change starts or stops it.

**The audible identity is the pair (embedded stream, show layer).** A scene
change is an audio no-op exactly when that pair is unchanged:

- stepping through a show's slides compiles the same `audio` block, so the
  track keeps its position, clock and sink across the change (the M3
  continuity rule);
- a **visual-only** change — a different still, a colour, a gradient, a
  camera, or the same video with the same layer — keeps the player and only
  the picture is rebuilt, so a picture dropped onto a lyric cue does not
  restart the music;
- a change that alters the audible mix starts a new player: a different
  background video (its embedded stream is part of the mix) or a different
  show track. There is no cross-fade between two players; the new cue's track
  starts at its beginning, and the outgoing audio goes silent at the hand-off
  (6.22c's rule).
- moving between two shows that name the same track file continues it: the
  identity is the file, not the show id.

**Clear and Restore.** Clear Background and Clear Slide preserve the audio
layer (the clears are layer-scoped, and audio is its own layer), so the
audience keeps hearing the cue and the transport is untouched. Clear All is
the whole-output clear and does include audio: the cleared scene carries no
`audio` block, so the track stops and the audience hears silence. Restore
re-presents the retained cue, which starts its track again from the beginning
— a deliberate re-present, not a resume. `canRestore` and the Clear
All/Escape gates count the acknowledged audio layer as output activity
(`audioOnAir`), so a cue that is only audible is never labelled "Output
clear".

**Seek and loop** are operator intents on the live player, not scene state:
they survive a visual-only change because the player does, and they are
discarded with the player when the audible mix changes. Loop defaults to on
for cue media. **Mute and gain** are mixer state: the master mute and every
per-source row survive a transport rebuild, and the master mute is applied to
each instance as it is adopted.

**A linked refresh publishes only what it captured.** A plan carries the
catalog's record and content revisions and the destination show; publishing
rechecks all three, drops a stale plan and re-runs its id from a deduplicated,
catalog-bounded owed set (one run per published plan, each capturing the catalog
as it is now). A Refresh whose destination the operator removed is dropped
rather than re-added, and an Add that never published is prepared again. This is
what makes an edit that lands DURING a measurement converge instead of being
lost behind the one deferred slot it used to share with every other id.

Prepared songs begin with one intentional wordless cue supplied by `SongPlanner`,
not an empty catalog stanza. The import preview and service bridge share that
plan; the service adapter admits an explicitly marked blank only at the start
of a plan containing lyric slides. Existing slide group strings are exposed as
`sectionLabel` preview metadata. `SlidePreviewCard` gives sections muted caption
colours and a canonical label; each host suppresses consecutive duplicate
headings, retaining full lyric thumbnails and the service's opening name marker.
Audience live state keeps its existing red priority. No stored format changes.

**Clipboard import** (`openshow::importer::parseLyrics`) reads only
`QClipboard::Clipboard` on an explicit Paste, normalises CRLF/CR, preserves
Unicode and authored line order, treats blank lines as stanza boundaries, keeps
bracketed/bare section labels (prose stays lyrics), keeps a lone title before
the first section as metadata while preserving unlabelled/multi-line lyrics,
and moves a trailing CCLI/©/permission block
into the record's reporting fields instead of slide words. `paginateSong` then
measures each stanza whole, packs authored lines on overflow, delegates only one
over-long line to the shared `textflow` sentence/word split, never shrinks the
font, and — the operator's rule — starts every section on a new slide so a
chorus never fills the previous verse's leftover room. `SongPlanner` provides
the non-blocking worker measurement (content-keyed cache, one batch in flight,
latest-ask-wins). `SongImportPage` shows the measured worker cards and offers
one destination, Add to Library; `ServiceCatalogBridge` is the only component
that writes a song into the running order.

UI parts: `LibraryBrowser` (Songs/Presentations, title filter, drag sources) and
`CurrentServicePanel` (one running order, no service picker under the
operator's one-service amendment) sit permanently in `PlaylistPanel`. Drag
payloads are `catalog:<id>` (category Move, service Copy), `show:<id>` (service
add) and `entry:<id>` (reorder). `DragFeedback` provides an overlay title/action
badge with a pointer-aligned hotspot; service drops show an insertion marker
and category drops highlight their destination. Service reordering works in
Show and Edit and retains the dragged occurrence while edge scrolling.

Song rows display titles without CCLI labels; attribution stays in the catalog
and service copies. Clipboard import lives in the toolbar More menu, preserving
the originating page; Library has no duplicate Add/Import buttons. Service
selection carries an entry ID to Show, keeping all service slides visible while
selecting/scrolling to that occurrence. Library browsing remains separate.
Full-width rules separate service groups in existing grid gaps without changing
cue order or cell mapping.

The LRF operator fixes add shared `ListSelection` semantics and focused Delete
to both panes. `removePlaylistEntries` validates occurrence IDs and removes one
service batch; `CatalogController::removeItems` commits one durable catalog
batch before notifying the bridge. `removeLibraryShows` hides referenced
authored shows using the existing `scheduleOwned` flag and deletes unreferenced
ones in one document edit. Service content survives both forms of Library
removal; detached song copies are excluded from the Library projection. No
service or catalog format changes are needed. Keyboard Undo/Redo routes to the
focused pane. Operator Library Delete requires an explicit permanent-deletion
confirmation, with Cancel focused by default. Confirmed source identities and
catalog links are removed from older history snapshots so undo cannot resurrect
them, while unrelated history and playable service copies remain. In-flight
song preparation is cancelled if its Library source is deleted.


## WYSIWYG editing, slide ordering and insertion (milestone EW)

The Edit stage edits the words ON the slide, not in a separate field. A
selected text box enters a text session on double-click, Enter or the box
menu's **Edit text**; the mode is identity-bound to one show/slide/box and the
session commits its draft to the document through the ordinary debounce, so a
typing burst is one document history entry and the native field never keeps an
independent undo stack. Escape, clicking outside the box and focus exit commit
and leave; an inspector receiving focus keeps it. Navigation, a
selection change, a removed target, undo/redo, save and page leave retire the
session deterministically.

The visible words stay the worker's render. `InlineTextEditor.qml` contributes
only an input sink and chrome: a transparent, focused `TextInput` for IME,
clipboard, grapheme/word navigation and line breaks, plus a caret, selection
rectangles and composition decoration. Pointer selection is done with the
worker's hit test, so clicking and dragging select the glyphs under the
pointer. Up/Down use that geometry and hit test to move between rendered
lines, retaining a preferred horizontal position through short/empty lines;
Shift extends the selection and repeated keys wait for matching caret geometry.
When the worker cannot answer geometry — disconnected or too old —
the editor clears the chrome and Edit shows an explicit **Source text** drawer
(automatically and labelled as such when the capability is absent); it is
source editing, never a Qt layout pretending to be the audience's pixels.

`text.editor` (protocol minor 21, capability `text.editor.v1`) is the one
geometry owner. The ask carries a compiled scene, one text item, an optional
bounded draft and the caret, selection or pointer it wants answered.
`OutputSession::editorLayout` calls the same `SceneTextEngine::layoutInBox`
that paints: carets come from glyph cluster bounds, selection from the
paragraph's own range rects, hit tests from `getGlyphPositionAtCoordinate`, and
all coordinates are authored 1920×1080 units with the frontend's zoom applied
once. Offsets are UTF-16 code units of the authored item text (Qt's unit);
`SceneTextEngine::sourceToDisplayOffset`/`displayToSourceOffset` bridge them
through the same uppercase transform that renders, so an All-caps expansion
(`ß` → `SS`) still names an authored character. A reply is installed only for
the newest request of the current session; nothing is stored, prepared,
presented or cached, and a geometry ask can never move the audience.

The slide rail reorders with a visible grab handle: a ghost follows the
pointer, an insertion gap is drawn before/between/after cards, edge autoscroll
is bounded to one row per tick, and Escape, a release outside the lane, the
source's own gap or a model change during the gesture cancels with no edit.
The visible gap converts exactly once to `Documents::moveSlide`'s final-slot
rule. The row menu and the rail's keys offer Move up/down and Insert
before/after; the footer's **Insert slide** inserts after the selection
(position zero for an empty presentation). `DocumentController` wraps these as
one history entry each, validates explicit show/slide/anchor ids, keeps the
selected slide selected by id across moves and undo/redo, selects the fresh
slide on insert, and inherits the anchor's section, text context, background
and fit without copying words, notes, foreground items or per-slide style
overrides. A leading blank cue is an ordinary slide: an explicit position is
obeyed and the cue is neither moved nor recreated.

Evidence: `editor_layout_tests` (real worker wrap, draft override, capitals,
round-trip hit tests, refusals, latency); `frontend_journey_tests` (controller
adapter, output isolation); `library_service_ui_tests` (double-click/Enter
entry, typing, worker caret and selection, line breaks, Up/Down/Shift navigation,
click-outside/Escape and inspector focus handoff, undo
granularity, source drawer, insert→type→reorder→undo/redo on the real shell);
`document_controller_tests` (move/insert verbs, refusals, no-ops, inheritance,
leading cue, persistence); `workspace_ui_tests` (real rail pointer drags,
cancellation, autoscroll, menu/keys, empty insertion and the no-worker
fallback); `protocol_tests`/`rpc_ipc_tests` (minor 21 and the capability).

## Whole-item themes and optional reflow (milestone PT)

A theme can be applied to a WHOLE song or passage from its left-hand row, not
to one tile: the target is the clicked record, and the dialog names its title,
its occurrence count (a show listed twice is one presentation) and the
eligibility answer. **Apply only** is the default and keeps every current
break; **Apply and reflow** re-paginates the words the item already holds; an
unprepared Library song offers only **Remember theme** and never adds a hidden
presentation or cue. The Show header's default picker and the cue tile's theme
menu are gone, so every item-level apply goes through the same prevalidated
transaction; per-slide overrides remain an Edit-page choice.

The remembered look is a VALUE, `domain::ThemeSnapshot` (`themesnapshot.h`):
the complete resolved appearance, both alignments, All caps, spacing, weight/
slant/underline, fit/minimum, stroke/shadow, primary placement, resolved
movement and named contexts, captured concrete through the Theme Editor's
fixture with the service current theme excluded. No service id, asset path or
backdrop media travels. Catalog format 2 stores it optionally on a song; a
malformed value refuses through `Documents::validThemeSnapshot` before any
durable write, canonical words are untouched, and re-setting the same value is
a no-op. Applying materialises it as a service-local theme by full-definition
fingerprint (name and id aside), never overwriting a same-named different look.

Service format 15 carries `Show.appliedThemeId` and `Show.reflowSource`. With
an explicit assignment the theme chain is slide theme → applied item theme →
Default, with slide customizations still winning per field and the transition
resolver walking the same branch; without it the legacy current → slide → show
default chain is the exact code path every earlier service used. Applying
clears every slide's old slide theme and TEXT customization through the shared
`textCustomizationFields()` classification (colour, family, size, alignments,
spacing, caps, weight/slant/underline, fit/minimum, effects and the placement
override) while preserving backdrop colour/gradient, background asset/fit/
camera, authored item geometry/order, words, notes, groups, IDs, counts and
media/audio references. "Use service default" removes the assignment and the
target's legacy show-default and slide themes only.

Reflow reads `reflowSource`: a closed kind, ordered blocks with section/verse
labels and original-case lines, scripture provenance and a fingerprint of the
service text projection it describes. A word, break, order or section edit
invalidates the fingerprint; a style-only edit does not; undo/redo and save/
reopen carry words and source together. A legacy or edited item falls back to
the conservative current-word source, with unlabelled scripture blocks naming
the whole passage rather than an invented verse range. Nothing in a reflow can
read the Library or a provider.

Eligibility is one sentence (`domain::reflowEligibility`): one text box per
content slide, no foreground picture beside words, at most one leading blank
cue, no internal/trailing wordless cue, and one common background/fit/text
context. Songs keep the section-starts-a-new-slide policy; passages pack
greedily with the import's measured caption reservation. `ui::ReflowPlanner`
captures the chosen look's box (from the snapshot, never the browsing cursor),
the source, the projection fingerprint and the document generation, measures
asynchronously in bounded rounds, and refuses a reply whose item vanished or
whose words moved. `ui::ThemeApplySession` orders the two stores: prevalidate,
write the Library preference durably, then publish ONE service edit (look plus
replacement slides); a refused write or stale plan publishes neither, and the
two undo scopes stay separate. No cross-store crash atomicity is claimed.

Evidence: `domain_tests` (snapshot capture/codec/limits, the item chain,
preservation, service-15 read/write, projection fingerprint, eligibility rules,
both fallbacks); `catalog_tests`/`catalog_controller_tests` (durable preference,
revisions, restart, separate signal); `reflow_tests` (both pagination policies,
splitting, reservation, word conservation); `document_controller_tests`
(adapter verbs, snapshot/measure, A→restart→B, source capture/refresh);
`theme_apply_tests` (two-store transaction, one-undo restoration, stale and
refused paths, separate scopes); `reflow_planner_tests` (real worker: roomy vs
tight look, prepare/commit/undo, stale words, no worker, and the PT.8
Library→service→reflow→Go→save/reopen journey with legacy scripture and media/
override preservation); `workspace_ui_tests` (the row actions, the dialog,
keyboard menus, the removed bypasses, no-worker and ineligible paths). The
PLAN ledger closes PT.1–PT.8; physical gates (projector, NDI, target-GPU)
remain recorded separately under M7/ND/TA.

### Transparent presentation content (2026-11-07)

Themes no longer supply backgrounds. Format 11 saves only foreground theme
settings and migrates formats 1–10 by retiring theme fills/gradients. Slide-owned
backgrounds and assets retain their authored behavior. Scene 5 carries explicit
`background.transparent`; scenes 3/4 remain readable with their old opaque
semantics. The worker preserves RGBA for off-air pictures and retains the active
background when replacing it with transparent presentation content. The Qt
preview paints a checkerboard behind alpha; it never reaches worker scene
content or audience frames. Details and regression evidence:
[acceptance](reviews/transparent-presentations-2026-11-07.md).
