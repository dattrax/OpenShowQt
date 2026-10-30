# M6 code and architecture review

Historical baseline review. Many findings below have since been fixed; use
[PLAN.md](../PLAN.md#m6--code-review-remediation-and-architecture) for current
task status and the [2026-10-29 architecture audit](architecture.md) for current
boundaries and unresolved differences. Original observations/evidence follow.

Reviewed: 2026-10-25 (Europe/London). Baseline: `1f44b7e` plus the existing
six-line transport-clock log change in `OutputSession.cpp`. That working change
was preserved. This review changes documentation only; the fixes below are TODO.

The implementation has reached the M5 checkpoint, but several behaviours marked
complete have only been proved at component/benchmark level. M6 now closes these
correctness, architecture and scope gaps. Productization and the former M6
reliability/packaging/rehearsal tasks move to M7. See the executable task ledger
in [PLAN.md](../PLAN.md#m6--code-review-remediation-and-architecture).

## Assessment and evidence

The process split is sound: documents and writes belong to the frontend; decoding,
audio, Vulkan and Skia belong to the Qt-free worker. The domain, scene and protocol
libraries are useful boundaries. Bounded frame/audio rings and skipped layers are
worth retaining. A rewrite, an additional service, or a generic plugin framework
would add work without addressing the observed problems.

The largest risks are incorrect state under rapid operator actions, unsafe frame
ownership, audio reuse, recovered-media lifetime, and blocking preparation. File
length exposes these mixed responsibilities, but splitting files alone will not
fix them. Introduce a component only around an explicit owner, thread or lifecycle.

Verification in this review:

| Check | Result and limit |
|---|---|
| `CCACHE_DISABLE=1 cmake --build --preset headless -j 2` | Passed; rebuilt the current worker |
| `ctest --preset headless --output-on-failure` | 16/16 passed in 25.78 s |
| `CCACHE_DISABLE=1 cmake --build --preset desktop -j 2` | Passed |
| `ctest --preset desktop -LE display --output-on-failure` | 21/21 passed in 25.93 s; excludes the five display-labelled checks |
| Small standalone probes against current production sources | Mixer restart refused; future preview release reclaimed an owned slot; sender emitted a frame its receiver rejects as oversized; recover→save→clear removed the live asset source and the second save failed |

The first build attempt hit ccache's read-only cache directory; disabling ccache
resolved it without changing the project. Vulkan tests used **llvmpipe**, not the
target RADV device. The live PipeWire subscenario skipped internally because the
daemon connection was denied. These passes do not establish hardware timing,
live audio, projector placement, Wayland/X11 acceptance or a service soak. No
display-labelled tests were run during this review.

P1 means fix before relying on the affected live-service behaviour. P2 means a
correctness, maintainability or promised-scope gap that must close before the
alpha. Source inspection is distinguished from the four runtime probes; most
concurrency/failure scenarios below still need the named regression tests.

## Findings, ordered by consequence

### R1 — P1: production output still uses the development scene (6.1a–b)

`OutputSession.cpp:107` always creates `TestScene`. `TestScene.cpp:333` builds the
demo photo/title and leaves the clock enabled at line 367. Before an active scene,
the layers draw the demo. The clock layer at line 273 paints even with an active
scene, including black clears, and `OutputSession.cpp:1038` always supplies the
development device/frame status line. Preview supplies an empty status line at
line 905, so audience and preview are different compositions.

This violates black startup/restart, production clock-off defaults, preview
consistency, and the explicit M3 graduation of TestScene into a fixture. Extract
the production compositor and its transition state; provide an empty black
scene in socket mode. Keep demo layers, procedural photo, CPU snapshot helpers
and composition assertions in development/test targets. Prove fresh startup and
clear-all are uniformly black, including the footer region.

### R2 — P1: repeated audio starts and standalone-only playback fail (6.7a–b)

`AvPlayer.cpp:153` stops feeders, joins the mixer and stops the sink, then only
re-arms the player's own `fStarted`. `AudioMixer.cpp:28,91` gates input creation
and start on its own `fStarted`, which `join()` never clears. After the first
audio-bearing background, a later start cannot register its input; it can return
success with no feeder and lose audio silently. Sink readiness/activation state
also survives `PipeWireSink::Impl::stop()` (`PipeWireSink.cpp:266`).

Runtime probe: first mixer input `0`, first start `true`; after stop/join,
`addInput()` returned `-1` and `start()` returned `false`, "AudioMixer is already
running". Recreate or explicitly reset the entire transport state, not just one
flag; cover audio→audio, audio→silent→audio and failure→retry.

Separately, `AvPlayer.cpp:59` expects the standalone input to have index 1 even
when input 0 was never registered. A standalone-only configuration registers 0,
fails `inputsOk`, and starts no feeder. Use stable source identities independent
of which other source exists. The fallback clock at line 270 is elapsed wall
time: pause and seek do not update it, so `clockSeconds()` does not obey transport
semantics without live audio. Verify position, pause, forward/backward seek and
resume in the sinkless path as well as the audible path.

### R3 — P1: rapid scene requests can show the wrong slide (6.3a–b)

`WorkerController.cpp:282` identifies replies by result shape and uses one
`fPendingPrepareJson` and one `fAutoPresentScene` for every request. The request
IDs generated at line 563 are not used to correlate responses. Two calls A then B
before A's acknowledgement overwrite the pending JSON with B. A's reply mirrors
B under A's revision and consumes the auto-present flag; B's reply does not
present. The audience can get A while the UI believes its revision describes B.
Errors leave the pending intent unresolved as well.

Track request ID → operation, payload, connection generation and presentation
intent; define latest-go-wins explicitly. Then compose clear commands against
the pending logical output state: `clearSlide()` followed immediately by
`clearBackground()` currently derives both from the last polled active scene,
so the second can restore the words the operator just cleared. Pin restore
content until its acknowledgement; pruning currently protects neither the
worker's restore revision nor the UI's mirror, and `restoreOutput()` drops the
restore point before success.

### R4 — P1: status acknowledges state before a frame presents it (6.4)

`OutputSession.cpp:632` publishes active scene and the latest global revision
before transport startup and before `frame()` succeeds. Layer/display apply
methods similarly assign `fStatus.revision = fRevision` (`569,606`). A command
arriving between the run-loop capture and these assignments can be credited to
an earlier frame. Failed acquire/present still leaves the scene advertised live;
`scene.pending` is never cleared. Preview tags use the latest desired revision
at line 850 rather than the rendered snapshot.

Stage one immutable command/frame snapshot, and publish its applied revision and
scene after successful submission/presentation according to a documented rule.
Keep queued, transitioning and confirmed state distinct. Tests must inject a
command during apply and an acquire/present failure, not just poll until counters
increase. Existing present→air latency measurements use this early status and
therefore need revalidation against actual frame content.

### R5 — P1: preview release does not prove ownership (6.5)

`PreviewStream.cpp:163` accepts any release sequence newer than its watermark;
it need not match the frame currently published. Runtime probe: slot 0 published
sequence 2, a release of 102 immediately made that still-owned slot claimable.
`preview.ready` and `preview.released` contain no stream generation/name, while
`OutputSession.cpp:390` applies releases to whichever stream is current. A delayed
release from an old stream can free a new stream's slot. Stream replacement can
also publish old notifications after `startPreview()` clears the queue.

Carry stream identity with ready/release messages, accept only the exact current
published sequence, and reject stale/future/odd releases. Validate mapping
geometry and arithmetic before mmap/copy. Exercise stop/start with outstanding
frames and delayed releases; the consumer must never see its owned slot changed.

### R6 — P1: a held video token outlives its ownership (6.6a–b)

`OutputSession.cpp:1006` retains `fDrawnVideo` for redraws, but line 1143 calls
`onPresented()`, whose uploader release sets `inUse=false` (`YuvGpu.cpp:795`).
Decode can overwrite the optimal image while the session still treats that token
as the held frame. `VideoSubmission::sequence` is not checked when reacquiring
the image. A dirty redraw/preview can sample a later frame early, and the preview
only branch at `OutputSession.cpp:1293` lacks the outer graphics-queue guard used
by the main draw. The synchronous readback does not by itself prevent a new
upload from starting before that readback is submitted.

Give the displayed image a lease retained until replacement, with a separate
GPU-completion obligation for every sampling draw. Test a paused/final frame
while the decoder attempts reuse and while layers/previews redraw it.

Teardown also clears `fVideoPresenter` and pending submissions before stopping
the pacing producer (`OutputSession.cpp:780–789`), whereas `submitVideo()` reads
the raw presenter outside its lock (line 478). Stop/join or detach the producer
through a synchronized generation boundary before changing/destroying its
presenter. Pin concurrent swap/submit/disconnect with a deterministic test.

### R7 — P1: saving recovered media deletes the active source paths (6.12)

Recovery loads asset `sourcePath`s from `autosave/assets` (`storage.cpp:641`).
`DocumentController::save()` copies them into the service but does not rebind
the document or history paths, then removes the autosave tree (`938–950`).
Subsequent presentation, save or autosave can refer to deleted files, even though
the UI just reported a successful save. Ordinary import→save also continues to
depend on the external original for the live document until a reload.

A standalone probe of that same storage sequence confirmed the first save
succeeded, the live source disappeared after clear, and the second save failed
with "has no readable file". The probe used temporary fixture bytes; this
establishes path lifetime, not image-decoder behaviour.

Adopt durable package-owned asset references after save without losing undo
semantics; ensure snapshots and compiled/prepared references cannot point into
deleted scratch storage. Test recover→save→present→edit→undo/redo→save with real
media and after the original import source disappears.

### R8 — P1: save atomicity protects only the manifest (6.13a–b)

`storage.cpp:580` overwrites existing asset destinations before manifest commit.
An interrupted copy can corrupt bytes still named by the old valid manifest.
The manifest stream is checked before close, not after flush/close, and no file
or directory durability barrier exists before success (`594–612`). The header's
claim that an interrupted save leaves a loadable previous package is stronger
than the implementation.

Stage assets under new immutable/content identities and commit the manifest only
after all referenced bytes are durable. Surface flush/close/rename failures and
define the power-loss guarantee. Fault-inject failed copies and commits against
an existing package and verify the previous package remains usable.

Loaded asset paths accept absolute paths and `..` (`storage.cpp:441,641`), and
loaded IDs are merely nonempty/unique (`311`), yet `assetFileName()` trusts an ID
as a filename prefix. Enforce package-relative containment, including symlinks,
and safe opaque IDs/filenames. This enforces the self-contained storage design,
not a change to authorized external imports.

### R9 — P2: private Skia dependencies and an ICU impersonation remain (6.2a–b)

Direct application dependencies:

| Location | Internal API |
|---|---|
| `VulkanContext.cpp:18–19,344` | `src/gpu/GpuTypesPriv.h`, private VMA factory and `ThreadSafe` |
| `Swapchain.cpp:16` | `src/gpu/GpuTypesPriv.h` (appears unnecessary for its public-type use) |
| `SceneText.cpp:38–45` | private `SkTArray`, `src/base/SkUTF`, `modules/skunicode/src/SkUnicode_hardcoded.h` |
| `SceneText.cpp:679` | application defines the upstream `SkUnicodes::ICU::Make()` symbol |
| `tests/gpu_yuv_tests.cpp:38`, `tests/gpu_decode_tests.cpp:26` | `src/core/SkYUVMath.h` for expected conversion values |
| `OpenShowSkia.cmake:49` | exports private/base and `src` include directories to every consumer |

Use the public `include/gpu/vk/VulkanMemoryAllocator.h` contract for an owned
allocator adapter (or a verified public allocator factory in the selected Skia
build). Never copy private declarations or redeclare internal factory symbols
to hide the include. Use public SkParagraph/SkUnicode module entry points with a
compatible ICU build. These headers' own transitive private includes are Skia's
implementation detail; the enforceable rule is no **direct application** include
or symbol use from Skia's private/src trees. Remove broad private include paths,
add an allowlist check, and pin/validate the header and library revision together.
Public APIs still require controlled upgrades; they are not an ABI guarantee.
Include tests in the direct-private-header guard. Replace their private YUV
matrix helper with independently specified conversion expectations; copying an
upstream implementation or testing it against itself defeats this boundary.

The homemade Unicode backend returns identity bidi ordering, treats every code
point as a grapheme start, and implements only whitespace wrapping
(`SceneText.cpp:66–234`). Font fallback is not evidence of Unicode segmentation
or bidi correctness. Replace it, remove the ICU symbol shim, and add combining
marks, emoji sequences, CJK wrapping and mixed-direction text fixtures. Treat
the old M3 ICU deferral as outstanding work, not accepted typography completion.

### R10 — P2: preparation blocks RPC and presentation (6.9a–b)

`scene.prepare` synchronously calls image decode or `probeMedia()` on the only
RPC thread (`OutputSession.cpp:272`). Releasing the session mutex does **not**
allow another socket command to be served while that call runs. Clear, shutdown,
status and preview releases all wait behind the file. On presentation, video
probe/open, direct-decode priming, allocations, audio connection, up-to-2 s
pre-roll and old-transport joins occur on the render thread
(`OutputSession.cpp:651–790`, `AvPlayer.cpp:107`). Preparing video only probes;
it does not make the expensive transport ready. Text layout is also cold in
`WordsLayer::draw()` (`TestScene.cpp:212`).

Add a bounded preparation executor with explicit ready/failed/cancelled scene
state. Keep the poll loop responsive; hand the renderer prepared resources at
a short apply boundary. Limit the number/bytes of primed video resources and
retire old transports away from frame presentation. Keep Ganesh calls on its
owner thread. Verify slow files, superseded prepares and disconnect cancellation;
measure video as well as still-slide present latency and ongoing output cadence.

### R11 — P2: control transport limits are incomplete (6.8a–b)

There is **no decoded-media transfer in the current RPC design**. The complete
route audit is below. However, the 1 MiB receive limit is not a send limit:
`protocol.cpp:55` serializes and casts the size without checking it. The runtime
probe emitted 1,048,650 bytes including framing; its receiving decoder rejected
the 1,048,646-byte payload. Pasting sufficiently large text can disconnect the
worker rather than return a local validation error. Scene item/line/string counts
and unknown fields have no application budgets (`scene.cpp:361`).

Enforce encoded-size limits at both senders, and negotiated per-method budgets
for scene text, counts and asset references before enqueueing. Reject media byte
fields/data URIs as inputs; keep legitimate local paths. Measure representative
small and worst-supported control messages; do not arbitrarily shrink the limit
below the supported text/scene model.

`RpcServer.cpp:36` uses blocking `send()` in an otherwise polled socket loop.
A client that stops reading prevents command handling and disconnect detection.
The UI keeps polling into `QLocalSocket` without an in-flight bound. Use
nonblocking writes, a bounded output queue, POLLOUT, progress deadlines and
coalesced status/preview notifications. No lock held by a slow peer may block
rendering. Also validate integer ranges **before** narrowing preview/display
parameters, enforce JSON-RPC ID/notification shapes, and remove the unsynchronized
cross-thread `fConnectionFd` access (`RpcServer.h:70`, `stop()`/`run()`).

### R12 — P2: decoded-image memory grows for the worker's lifetime (6.10)

`SceneImageCache::imageFor()` reads a whole file into a string, copies it into
SkData, allocates its full decoded dimensions, and retains every path forever
(`SceneText.cpp:486–522`). The 16-scene store and 64 MiB Ganesh cache do not bound
these CPU images. A 3840×2160 RGBA image alone is about 31.6 MiB; many distinct
backgrounds exceed the target machine's budget. Null failures remain cached
forever too, so repairing a file at the same path does not recover.

Use byte-accounted eviction with pinned active/transition/prepared resources,
encoded-size/dimension limits, explicit invalidation and retryable failures.
Keep misses/decodes outside draw. Test a sequence exceeding budget plus a
missing→repaired image at the same path, and expose cache usage in diagnostics.

### R13 — P2: preview mutates live transitions and image fades are not a lerp (6.11a–b)

`TestScene::draw()` advances the shared transition state at line 541. Preview
calls draw again with a later timestamp, including preview-only ticks; it can
complete a fade before the audience sees that blend. Use one frame snapshot
(scene, time, revision, transition blends) and make drawing to a target read-only.

For stills, `drawSceneBackground()` applies alpha separately to the incoming
colour fill and incoming image (`SceneText.cpp:530,572`). With an opaque image
and blend 0.5 the result is 0.25 old + 0.25 incoming fill + 0.5 incoming image,
not 0.5 old + 0.5 incoming image. Even the same image can dim during a lyric
change. Existing solid-colour midpoint tests miss this. Add image→image and
same-image transition pixel checks and implement the promised composite math.

Text fades call `saveLayer()` at `SceneText.cpp:471`, contradicting the fixed
single-pass/no-intermediate-target rule. The plan's M3 row disclosed this but
did not reconcile the fixed rule. Use a public paragraph paint/update or painter
adapter path that applies alpha during text drawing, and benchmark it. Keep the
single-pass constraint explicit; hiding an intermediate target in a helper does
not satisfy it.

### R14 — P2: recovery writes block the UI and errors are discarded (6.14a–b)

Every successful edit and undo/redo calls autosave synchronously on the UI thread
(`DocumentController.cpp:152,1011,1042`). `Autosave::tick()` re-saves the whole
package, copying external media again after text changes (`storage.cpp:677`).
Large videos can freeze navigation. The returned `tickError` and `clearError`
are ignored. A stale autosave can reappear after an apparently successful save
or discard. The 600 ms pending text edit is not flushed by either `Main.qml:34`
or the `aboutToQuit` hook (`main.cpp:66`), risking loss of the final keystrokes.

First make failures visible and flush pending edits on normal close/signal exit.
Then move persistence of immutable document snapshots to one bounded/coalescing
save worker, reuse unchanged asset copies, and acknowledge completed durability
back on the UI thread. Prevent an older in-flight autosave from overwriting a
newer save/discard decision. Test slow storage, write refusal and close during
the debounce window.

### R15 — P2: some promised features exist only below the production boundary (6.15a–b)

The RPC router exposes hello/ping/status/displays/select/layers/scenes/previews/
shutdown, but no play/pause/seek/loop or per-source gain/mute operations.
`AvPlayer` and benchmark flags expose them internally; the operator cannot use
them in socket mode. `secondAudioPath` is set by the benchmark, never by the
production session. Production backgrounds always loop.

Graduate the existing transport into session commands, a shared typed protocol
contract, acknowledged status and UI controls. Add a standalone track lifetime
independent of slide/video replacement; audio stays outside the composition
layers. Cover audio-only, embedded-only, mixed, per-source mute/volume and final
frame/loop-off through the actual UI→RPC→worker path. Do not count a passing
component benchmark as proof of operator functionality.

### R16 — P2: editor imagery and thumbnails do not follow the planned renderer (6.16a–b)

`EditCanvas.qml:139` draws QML `Text` with independent wrapping/font fitting.
The sole shared-memory stream mirrors audience output; there is no operation
to render a selected unpublished scene. The live preview therefore cannot
validate an off-air edit without presenting it. Slide list previews are strings,
not worker-rendered thumbnails (`DocumentController.cpp:121`, `Main.qml:961`).

Add bounded worker render requests for selected prepared scenes and thumbnails,
with target/request generation in the metadata and the same shared-memory pixel
transport. QML should draw only handles/selection over that imagery. Editing or
requesting thumbnails must not change audience scene, playback or transitions.
Cancel/coalesce requests as selection changes; do not send original images or
decoded video to the UI as a substitute.

### R17 — P2: initial-scope authoring remains narrower than the plan (6.17a–c)

`domain::Slide` has one lines/geometry tuple and one background asset
(`documents.h:41`); `compileSlide()` creates at most one text item
(`scene.cpp:279`). The wire renderer can draw several text items, but the editor
cannot author/persist them. There is no authored foreground image item or
layer ordering; `ImageLayer` is still a demo-only picture. Production image
backgrounds always use cover; gradient/background fit choices in the agreed
scope/reference are not represented in the model.

Close these in separate vertical slices: stable multi-text-box IDs and editing;
foreground image items with geometry/order; background gradient and supported
fit modes. Each slice includes schema migration, storage, compiler, worker and
UI acceptance. Split further before coding if a slice exceeds two days. Preserve
old single-box services exactly. No advanced effects, additional outputs or
other explicitly deferred features are added by this review.

### R18 — P2: remaining failure handling is visibly incomplete (6.7c, M7.1a–c)

Image decode failures return null without media.error; video preload failures
only print to stderr (`OutputSession.cpp:298`). Running pipeline fatal errors
and EOF are retained in `PlaybackPipeline::stats()` but never reconciled with
`status.media.playing`. Expected embedded audio can fail to open/activate while
`AvPlayer::start()` still returns success, and media status claims success.
Promote these failures through typed state without claiming media is on air
before a usable frame/stream exists (6.7c).

The existing reliability TODOs remain required M7 work: display refresh merely
replaces an index list (`OutputSession.cpp:513`), with no removed-target disable;
the UI fetches displays once after hello. Device loss goes through the same
swapchain recreation path as resize, several Vulkan/Skia return values are
ignored (`Swapchain.cpp:273,304`), and acquire/fence waits are unbounded.
`WorkerController::start()` has no complete restart reset for its decoder,
handshake/tracking/timers, shutdown flags or stale preview image. Add bounded
failure transitions and restart black with documents intact and explicit go.
The former M6 tasks were never DONE; moving them to M7 does not waive them.
Diagnostics also need honest metrics: `PlaybackPipeline::onPresented()` counts
held-frame redraws as presentations, and `driftMaxMs` keeps only positive drift.
M7.2 must distinguish unique video sequences from redraws and report signed and
absolute A/V timing, rather than treating these counters as a complete timing
oracle.

### R19 — P2: seek acknowledgements and frame deadlines disagree (6.7d)

The video decode loop captures a seek target, performs the seek, then marks the
**latest** requested generation applied (`VideoFrameSource.cpp:743–764`). A
second request arriving during `av_seek_frame()` can be acknowledged without
ever applying its target. Store the generation paired with the target actually
applied and leave newer work pending. Audio has a separate generation protocol;
`AudioFeeder.cpp:245–247` ignores the boolean result of `applyPendingSeek()` and
still acknowledges failed seeks as applied.

`PlaybackPipeline.cpp:224` computes a re-anchored deadline, but its submission
at line 262 sends raw `pts` while the session compares that field directly with
the transport clock (`OutputSession.cpp:1003`). Thus the pacing layer and session
use different deadlines after pause/seek offsets. Define media position and
clock deadline as separate fields (or use one shared mapping), propagate seek
failure, and test rapid seeks, backward seek after looping, and paused seeks
through the actual session rather than only a collecting SubmitFn.

## RPC and media transfer audit

> **Updated 2026-10-30 (task 6.20b).** The route inventory below was the
> review-time observation; its payload column now states what the shipped
> code enforces, from `src/protocol` (shared by both sides),
> `src/scene/scene.h`, `src/worker/rpc/{OutboundQueue,RpcMethods}.cpp` and
> `src/ui/RpcClient.cpp`. The original claims ("size is currently
> insufficiently bounded", "string/output bounds need enforcement") were
> true at `1f44b7e` and are what 6.8a–b closed.

| Route | Current payload | Pixels/media bytes? |
|---|---|---|
| UI→worker `hello`, `ping`, `shutdown` | Version, client name, request ID | No |
| UI→worker output/layer controls | Display index, layer name, boolean | No |
| UI→worker `scene.prepare` | Text, resolved style/geometry, IDs, local asset path, kind | No. Bounded both ways: every frame carries `kMaxMessageBytes` = 1 MiB, enforced at send (`appendFrame` refuses locally, the connection stays up) and at receive (`Decoder::next`); nesting ≤ `kMaxParseDepth` = 8 (a `scene.prepare` counts 5); application budgets before enqueue — ≤ 8 items/slide, ≤ 256 lines/layer, ≤ 1024 chars/line, ≤ 16 KiB/layer, ≤ 64 KiB/scene, IDs/asset paths ≤ 512 chars, scheme-looking refs (`data:` included) refused as asset paths. The largest every-budget scene measures 39% of the 1 MiB frame (`scene_tests` pins it) |
| UI→worker `scene.present` | Prepared revision, optional transition kind | No |
| UI→worker preview controls/releases | Geometry/fps or slot/sequence | No. `preview.start` integers validated before narrowing; ≤ 64 in-flight requests per connection on the UI side |
| Worker→UI results/status/displays/errors | Small control/state metadata, paths, errors | No. Same 1 MiB send budget; method names ≤ 64 bytes; any echoed client text clipped to `kMaxEchoedTextBytes` = 160 at a UTF-8 boundary, so the "this key is too long" reply cannot itself be too long; per-frontend outbound queue `kMaxQueuedBytes` = 1 MiB / `kMaxQueuedFrames` = 1024 with nonblocking writes and a stall deadline |
| Worker→UI `preview.ready` | Slot, sequence, dimensions, stride, revision, timestamp | No |
| Worker→UI shared-memory segment | Three composed/downscaled RGBA preview slots | **Yes: authorized preview renders only** |
| Worker internal decode/upload/mix | FFmpeg frames, Vulkan images, F32 audio | Stays inside worker; not RPC or UI transport |

There is no RPC media-upload method, base64 image/video serializer, or frontend
decoded-media sender in this codebase. `QImage` receives **rendered preview** bytes
after mmap, which is appropriate. Import/save copies media files on disk; this
is the frontend-owned persistence operation, not a decoded-bitmap API transfer.
6.20b re-confirmed the finding against the method table (`RpcMethods.cpp`):
the wire vocabulary is control metadata, paths and the shared-memory
handshake, and no method accepts media bytes or a data URI.

The negotiated stream is bounded explicitly (6.8a/6.11a): `preview.start`
accepts width 64..1920, height 48..1080, fps 1..60, with the geometry
validated before any mmap or slot arithmetic (R5's rule); the UI asks for
640×360@15, and at that size a frame is 921,600 bytes, about 13.2 MiB/s,
outside JSON. The on-demand targets are bounded separately and cannot exceed
the negotiated slot: `preview.select` carries no geometry at all (it renders
the prepared scene into the same stream, pinned through the cache), and
`preview.thumbnails` accepts at most 32 entries with ≤ 128-byte keys at
32..512 px per side, each extent re-checked against the slot
(`thumbnailExtentFits`). Synchronous GPU readback still happens on the render
thread after the present, now inside `PreviewProducer::fillPreviewSlot` with
a budget that defers later attempts after an overrun; that limit, its cost
measurement and the audience-priority numbers belong to the open-differences
row in [architecture.md](architecture.md) and to **M7.2**, not to this
table's silence.

## Decomposition targets

Counts at review time are evidence of mixed responsibilities, not size limits.
Do correctness fixes with regression coverage first; extract one ownership
boundary per subsequent task and keep behaviour unchanged during extraction.

| Current file | Lines | Target boundary |
|---|---:|---|
| `OutputSession.cpp` / `.h` | 1312 / 463 | Thin render-loop coordinator; separate SceneStore/command snapshots, background transport owner and PreviewProducer; device/window/swapchain stay in existing output module |
| `TestScene.cpp` | 889 | Production SceneRenderer/scene layers/transitions; demo and verification harnesses in development/tests (6.1) |
| `SceneText.cpp` | 681 | Text engine and public Unicode/font setup; image cache/decode service; snapshot encoder in test/tool code |
| `RpcServer.cpp` | 619 | Poll connection/framing, bounded writer, method/parameter dispatch and status codecs; a narrow session command interface |
| `WorkerController.cpp` | 744 | WorkerSupervisor, ID-correlated RpcClient, PreviewConsumer, acknowledged output model; keep QML-facing facade |
| `DocumentController.cpp` | 1050 | DocumentSession (single history/dirty/navigation owner), save coordinator, reusable list/model adapters and focused edit commands |
| `Main.qml` | 1705 | Show workspace, editor/style inspector, output controls, settings page, shortcut routing as components with explicit inputs/signals |
| worker `main.cpp` | 805 | CLI/startup wiring; benchmark and diagnostics drivers under separate development sources/targets |
| `VideoFrameSource.cpp`, `YuvGpu.cpp`, `storage.cpp` | 819 / 889 / 719 | Retain cohesive decoder/uploader/codec owners; extract allocator/pixel-format/package-commit helpers only where the fixes create a real boundary |

Do not introduce one manager per method, a second mutable document store, or a
second text renderer. Keep CPU snapshot tests useful, but add tests at the real
WorkerController/RPC and worker render/presentation boundaries: existing tests
largely drive components or raw RPC and therefore missed several integration
failures above.

## Completion rule

Every R-number maps to focused M6 work in PLAN.md or an explicit M7 reliability task;
the plan records dependencies and acceptance. Fixes are not implied by this
review's DONE status. M6 closes only when the new behavioural regressions pass,
the direct-private-Skia guard passes, API traffic is measured, and the old gates
remain green with skipped hardware checks recorded honestly. M7 then validates
failure recovery, target-machine timing, clean-machine packaging and a full
service rehearsal.
