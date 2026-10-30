# OpenShowQt progress and handoff

Updated: 2026-10-30 (Europe/London).

## Current checkpoint

**M0–M5 are historical implementation checkpoints. M6 is incomplete.**
The committed checkpoint is task **6.19a3**, the frontend `PresentationModel`
(6.19a2's `PreviewConsumer` at `a4cb9e2`, 6.19a1's `RpcClient` at `001588e`,
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
**Next implementation task: 6.19a4** — the worker's connection loop versus its
method and status codecs, then **6.19b**. This handoff does not mark 6.20
complete.

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
- **6.19b/6.20:** background autosave is asynchronous; explicit save/load and
  decision/quit drains remain synchronous. Missing foreground-image failures
  are logged but lack acknowledged per-item UI reporting.
- **6.18b, 6.19a3–a4/b:** the remaining ownership extractions are unfinished
  (6.18a2's `PreviewProducer`, 6.18b1's `BackgroundTransport` and 6.19a2's
  `PreviewConsumer` landed; 6.19a1's `RpcClient` and 6.19a3's
  `PresentationModel` landed 2026-10-30).
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

The working tree carries task **6.19a3**. Tasks **6.19a2** (`a4cb9e2`),
**6.19a1** (`001588e`),
**6.18a2**, **6.18b1**, **6.18b2** and **6.9a** are committed at
`0907877`, **6.9b** at `d83469e`; what those moved is recorded below as well as
in their commit messages:

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
**6.19a4**, the worker's connection loop versus its method and status codecs;
then **6.19b**, then **6.20**'s recorded architecture differences. Do not claim
M6 closure until 6.20 resolves those and the remaining gates pass.
