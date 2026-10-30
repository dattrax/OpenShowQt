# Post-M6 conformance review

Historical findings at 6.20e. For the re-review after their implementation, see
[the 6.21 architecture follow-up](M6-architecture-follow-up-2026-10-30.md).

Reviewed 2026-10-30 (Europe/London), against clean commit `0e64f0b` (6.20e).
This review changes documentation only. Historical DONE tasks and their test
evidence remain recorded; new corrective work is tracked as **6.21a–g** in
[PLAN.md](../../PLAN.md). The previous plan and progress are preserved in
[history](../history/README.md).

The main architecture matches the plan, but full M6 conformance is not established.
Seven findings remain, including three P1 issues. Passing the existing suite does
not cover the combinations below. Complete 6.21 before treating M6 remediation as
finished; the already-open M7 reliability and hardware gates still apply.

P1 means fix before relying on the affected live-service behaviour. P2 means a
correctness, visibility or validation gap to close before alpha acceptance.

## Findings

### F1 — P1: prepared layouts can be replaced while the render thread uses them

Locations: [SceneText.cpp:163](../../src/worker/render/SceneText.cpp#L163),
[SceneText.cpp:181](../../src/worker/render/SceneText.cpp#L181),
[SceneRenderer.cpp:225](../../src/worker/render/SceneRenderer.cpp#L225),
[SceneRenderer.h:239](../../src/worker/render/SceneRenderer.h#L239).
Owner: **6.21a**; affects 6.9b and R10/R16.

`LayoutCache::find()` and `put()` return a pointer into a mutable four-slot cache
after releasing its mutex. `SceneItemsLayer` draws through that pointer, while
`prewarmLayouts()` on the prepare executor can replace the same slot, including
its vector of items and owned Paragraph objects. The Scene's shared ownership
does not pin its layout. The cache's mutex protects lookup/replacement, but does
not protect subsequent use by `drawLayout()`.

A standalone probe linked against the current render library filled four slots,
held the reference for scene 0, then let a second thread insert four more layouts.
After joining that thread, the **same held reference described scene 7**, while
scene 0 was still alive. This safely demonstrates the lifetime defect without
deliberately executing a data race. Actual overlapping draw/replacement can race
vector/Paragraph destruction and cause incorrect output or a worker crash; that
crash was not induced in this review.

Return a retained layout handle with a lifetime covering the complete draw, and
keep replaced/evicted layouts alive until their consumers release them. Add
regressions for concurrent prepare/draw and same-scene replacement, rather than
only sequential cache hits and companion-cache isolation.

### F2 — P1: save adoption omits foreground-image paths from retained scenes

Location: [PresentationModel.cpp:171](../../src/ui/PresentationModel.cpp#L171).
Owner: **6.21b**; affects 6.12, 6.17b and R7.

`adoptAssetPaths()` rewrites only `background.assetPath`; it never visits image
entries in `items`. The same incomplete rewrite is used for the revision mirror,
in-flight prepares, desired content and restore snapshot. Saving recovered work
can copy a foreground image into the service and remove its old autosave source,
while these frontend payloads still name the deleted file. A subsequent restore
or derived presentation re-prepares the stale path. A fresh compile from Documents
can work, concealing the broken retained payload.

A probe drove the real `PresentationModel` through its injected transport seam:
present text plus a foreground image, adopt its old path, clear all, restore.
The in-flight prepare and restore both retained
`/old/autosave/assets/picture.png` instead of the adopted service path.
Existing path-adoption fixtures cover a background image only.

Rewrite every path-bearing scene item in addition to the background. Reproduce
recover → save → remove scratch → clear/restore with a foreground image, including
an adoption while prepare is in flight, against both the model and a real worker.

### F3 — P1: 6.9b still leaves audio startup and transport retirement on rendering

Locations: [OutputSession.cpp:884](../../src/worker/session/OutputSession.cpp#L884),
[BackgroundTransport.cpp:1048](../../src/worker/session/BackgroundTransport.cpp#L1048),
[BackgroundTransport.cpp:1066](../../src/worker/session/BackgroundTransport.cpp#L1066),
[BackgroundTransport.cpp:1186](../../src/worker/session/BackgroundTransport.cpp#L1186),
[AvPlayer.cpp:122](../../src/worker/playback/AvPlayer.cpp#L122),
[AvPlayer.cpp:158](../../src/worker/playback/AvPlayer.cpp#L158).
Owner: **6.21c**; affects 6.9b and R10.

The prime prepares the video's file half, not its audio transport. On the render
thread, `applyBackground()` still probes the file again and calls `fAudio.start()`.
That start opens audio feeders synchronously, can wait for PipeWire negotiation
(up to 1.5 seconds in `PipeWireSink.cpp:252`), and waits for pre-roll (up to two
seconds). Even a silent configuration attempts sink startup because `enableSink`
defaults true. The old background is stopped/joined/closed on this same thread.
Standalone replacement also calls stop/start synchronously at
`BackgroundTransport.cpp:580–606`, while holding the transport mutex, so it can
additionally delay RPC status readers.

These are source-confirmed waits in the normal prepared path, not merely the
documented fallback for an unprimed video. The slow-video-open fixture demonstrates
that particular open moving off-thread; it does not prove audio preparation or
retirement moving off-thread. No slow-daemon timing measurement was performed.

Finish staged readiness and retirement on a suitable executor, retaining explicit
quiescence, bounded ownership and render-thread GPU attachment/destruction. Test
slow audio open/pre-roll and slow old-transport retirement while observing audience
frame gaps. This is unfinished M6 scheduling work, separate from M7's device-loss
and bounded shutdown/recovery work.

### F4 — P2: clearing a gradient background leaves visible background pixels

Locations: [SceneEdit.cpp:41](../../src/ui/SceneEdit.cpp#L41),
[SceneEdit.cpp:65](../../src/ui/SceneEdit.cpp#L65),
[SceneText.cpp:1113](../../src/worker/render/SceneText.cpp#L1113).
Owner: **6.21e**; affects 3.4, 6.17c3 and R17.

`dropBackground()` resets the flat colour and image fields but leaves the optional
`background.gradient` field. The worker accepts that scene and draws a gradient
from black to the old bottom colour. `sceneContent()` also ignores the gradient,
so a black-to-colour gradient without an image is reported as having no background;
the Clear background button is disabled. After a clear the UI can claim output is
clear while the audience still sees colour.

The transform probe produced `gradient retained=1`, `content flag=0`; parsing the
cleared payload through the real scene codec succeeded with `hasGradient=1` and
bottom colour `ff0000`. The render branch confirms those retained values are drawn;
this review did not take a separate live screenshot of this failure.

Clear the optional gradient too and derive the live-content flag from the complete
background definition. Add black-to-colour and image-plus-gradient clear tests,
checking final pixels and acknowledged button state. Existing scene-edit fixtures
exercise flat colour/image backgrounds only.

### F5 — P2: cold thumbnail decoding still blocks the single RPC loop

Locations: [RpcServer.cpp:341](../../src/worker/rpc/RpcServer.cpp#L341),
[PreviewProducer.cpp:461](../../src/worker/session/PreviewProducer.cpp#L461),
[PreviewProducer.cpp:542](../../src/worker/session/PreviewProducer.cpp#L542).
Owner: **6.21d**; affects the scheduling contract, 6.9a/6.16b and R10/R16.

`RpcServer::serve()` calls `pumpThumbnailDecode()` synchronously, before collecting
notifications/deferred replies and reading new requests. The pump calls
`SceneImageCache::decodeAtMost()` on a cold file. One decode per tick bounds the
number of files, not the time spent reading or decoding one file. During that
call status, release, present, disconnect handling and shutdown processing wait.
The current cold-scene-prepare responsiveness fixture uses the new prepare
executor and does not test this separate thumbnail path.

Schedule thumbnail file work through a bounded executor with generation-aware
completion. Keep cache/GPU ownership explicit. Add a deliberately slow cold
thumbnail test that sends status/release/shutdown concurrently. This finding is
from source tracing; no slow-filesystem fault was induced.

### F6 — P2: foreground-image errors reach RPC status but never the operator

Locations: [RpcMethods.cpp:283](../../src/worker/rpc/RpcMethods.cpp#L283),
[WorkerController.cpp:453](../../src/ui/WorkerController.cpp#L453),
[PresentationModel.cpp:297](../../src/ui/PresentationModel.cpp#L297),
[OutputPanel.qml](../../src/ui/OutputPanel.qml).
Owner: **6.21f**; affects 6.20d and R18's visible-failure requirement.

The worker now encodes `scene.foregroundErrors` and `foregroundFailureEvents`.
The frontend status handlers never read either field, expose no matching QML
property, and show only background/standalone media errors. A missing foreground
picture therefore still disappears without a path/reason in the operator UI.
The new worker-status test establishes the wire report, not visible frontend
reporting. This is a source-confirmed end-to-end gap.

Expose the bounded foreground fault list/count, display the affected path and
reason, and clear the notice when repair is acknowledged. Preserve the independent
background-media fault lane. Cover the frontend projection and visible repair
transition, not only raw `output.status` JSON.

### F7 — P2: the production journey bypasses the frontend presentation controller

Locations: [production_journey_tests.cpp:377](../../tests/production_journey_tests.cpp#L377),
[production_journey_tests.cpp:647](../../tests/production_journey_tests.cpp#L647),
[production_journey_tests.cpp:684](../../tests/production_journey_tests.cpp#L684).
Owner: **6.21g**; affects the strength of 6.20c/e's closure evidence.

The journey uses the real DocumentController for authoring and a real worker for
pixels, which is useful integration evidence. However, go/clear/restore are sent
through hand-written socket helpers, not `WorkerController`/`PresentationModel`.
Restore explicitly re-presents `gold.scene`, the remembered worker revision,
whereas the product re-prepares retained content. It never exercises production
asset adoption or restore retention across worker-store pruning. Calling this a
complete production frontend journey overstates the evidence; those controller
behaviours are tested separately against scripted peers.

Add a real-controller/real-worker journey with pixel/status assertions, restored
content after pruning, foreground images, gradients and adoption after save.
Retain the raw protocol fixture for the lower-level behaviour it already proves.
Keyboard gesture automation remains a separately acknowledged coverage limit.

## Plan alignment beyond the findings

| Area | Assessment and evidence |
|---|---|
| M0–M2 build, ownership and state | CMake boundaries, Qt-free binary checks, domain/history/storage, scene compiler/store and protocol fixtures support the planned split. |
| M3 presentation and M4 authoring | Worker text/ICU, one authored mixed-item stack, independent kind toggles, editor preview and QML extractions are present. F2/F4/F6 expose missing combinations/visibility. |
| M5 media | Bounded decode/presentation queues, displayed-frame leases, typed transport controls and separate audio lifetimes are present. Two PipeWire streams are an explicit accepted decision, not an unresolved architecture mismatch. F3 limits scheduling conformance. |
| M6 ownership extractions | SceneStore, PreviewProducer, BackgroundTransport, RpcClient, PreviewConsumer, PresentationModel, RpcMethods, DocumentPersistence, DocumentViews and QML components exist and are wired. File extraction alone does not settle F1/F3/F5. |
| Rendering/dependencies | Public Skia/ICU build and positive/negative source guards exist; audience composition remains direct. The closure prose mistakenly named `render_layer_guard` as the private-Skia guard; corrected to `skia_public_api_guard`. |
| Persistence | Staged durable assets/manifest, containment validation, history adoption and background autosave are present. Explicit save/load/drains still block the UI by recorded decision; F2 concerns scene payload adoption after saving. |
| M7 | Restart/reset, device/surface failures, bounded GPU waits, stable hotplug identity, packaging/notices, resource budgets and hardware rehearsals remain TODO. They are not newly discovered M6 regressions. |

## Verification

Both existing build presets completed successfully (`ninja: no work to do`).
The headless suite passed **32/32 in 47.92 seconds** inside the sandbox; live
PipeWire subscenarios reported connection denial and skipped internally.
The desktop suite then passed **54/54 in 248.13 seconds** with local-session
access, including all **13 display-labelled** checks (137.54 sec*proc). Its GPU
logs identify **AMD Radeon 890M / RADV**, and live audio restart/sink scenarios ran.
Neither successful build required recompilation; these are checks of the reviewed
tree, with existing tests unchanged.

The initial sandboxed desktop attempt failed 15 checks. Socket/session restrictions
affected controller/display fixtures; concurrently running the headless preset also
collided with the image-cache fixture's PID-named temporary directory (`cannot
remove: Directory not empty`). After headless finished, the desktop rerun with
local-session access passed all 54. Serialize these presets in PID-isolated
sandboxes sharing temporary storage. The passing rerun does not establish the
untested combinations in F1–F7. Commands/environment are also summarized in
[PROGRESS.md](../../PROGRESS.md).

Two temporary standalone probes were compiled and linked against the current
production libraries using the compile/link settings of `scene_edit_tests` and
`scene_frame_tests`. They did not change production code or existing assertions:

```text
clear background: gradient retained=1, content flag=0
cleared scene accepted by worker codec=1, hasGradient=1, bottom=ff0000
in-flight image still references removed autosave=1
restore image path=/old/autosave/assets/picture.png
render acquired scene 0 layout: 0
same layout reference after prepare churn: 7; scene 0 still alive=1
```

The transform probe also confirmed Clear slide removes the complete foreground
stack, including images. That behaviour is consistent with the reference's Slide
clear, so it is not counted as a separate defect in this review. The two renderer
kind toggles and the Slide clear have different purposes.

No projector/unplug, audible two-stream pairing, thirty-minute A/V, ten-minute
deadline or one-hour service acceptance was performed. Passing automated checks
does not waive those M7 obligations or the findings above.
