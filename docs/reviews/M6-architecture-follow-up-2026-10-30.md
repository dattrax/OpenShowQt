# Architecture review after 6.21

Historical findings at 6.21g. The [review after 6.22](M6-post-6.22-review-2026-10-31.md)
assesses their implementations and records the current remaining issues.

Reviewed 2026-10-30 (Europe/London), clean commit `8402e69` (6.21g), against
the changes since `602958d`. This is a source and design review: no build,
runtime probe, test suite, audio capture or hardware test was run in this pass.
The earlier recorded 55/55 desktop and 32/32 headless passes remain historical
evidence, not verification of the scenarios identified here.

Several earlier fixes are sound, but the claim that all M6 architecture gaps are
closed is premature. Seven findings remain: four P1 safety/live-output issues
and three P2 scheduling/state/publication issues. PLAN.md tracks them as
**6.22a–g**. Existing DONE checkpoints are retained.

## Assessment of the previous findings

| Earlier finding | Source assessment at 8402e69 |
|---|---|
| F1 layout-reference lifetime | Addressed: cache lookup returns retained immutable layout handles and each draw holds them. A separate shared-font-cache race remains (A1). |
| F2 foreground path adoption | Addressed: the shared rewrite visits background and item paths across mirror, pending prepares, desired content and restore snapshot. |
| F3 render-thread startup/retirement | Partially addressed: held audio staging and off-thread joins exist. Retirement overlap, audible shutdown and remaining inline operations are unresolved (A2–A6). |
| F4 gradient clear | Addressed: clearing erases the optional gradient and live-content detection counts it. |
| F5 thumbnail RPC decode | The file read moved off RPC. Completion is not tied to the request's extent/renderer, creating a stale-result problem (A7). |
| F6 frontend fault visibility | Addressed: bounded path/reason projection is exposed to QML and displayed separately from transport faults. |
| F7 production controller journey | Addressed at the requested controller/worker seam: the new fixture drives WorkerController, PresentationModel and PreviewConsumer against a supervised worker. It does not establish audio retirement or GPU/thread safety. |

P1 means fix before relying on the affected live output. P2 means close before
alpha acceptance. The consequences below follow from source tracing; none is
presented as a crash, audible failure or delay reproduced during this pass.

## Findings

### A1 — P1: retained layouts still share a mutable font cache across threads

Locations: [SceneText.cpp:177](../../src/worker/render/SceneText.cpp#L177),
[SceneText.cpp:289](../../src/worker/render/SceneText.cpp#L289),
[SceneRenderer.cpp:356](../../src/worker/render/SceneRenderer.cpp#L356).
Owner: **6.22a**.

`LayoutCache::build()` calls the shared engine outside its slot mutex. Every
ParagraphBuilder receives the engine's same `fFontCollection`, and companion
renderers borrow that same engine. Preparation can therefore shape text while a
render-thread cache miss or new thumbnail also shapes text using that collection.
Making the engine method `const` and retaining the resulting layout do not make
the collection immutable.

The pinned Skia implementation confirms the shared mutation:
`third_party/skia/modules/skparagraph/src/FontCollection.cpp:115` reads
`fFaceCache->fTypefaces`, and line 155 inserts into that hash table without a
mutex. The ParagraphCache has its own lock; that lock does not protect the
separate font-face cache. Concurrent cold family/style resolution can race a
lookup or insertion, potentially corrupting memory or crashing the worker.

Give shaping contexts ownership of their own mutable FontCollection state, or
serialize all access to a shared collection through one documented owner. Keep
the same shaping implementation and share only resources whose thread safety
is established. Simply holding the layout-cache lock around shaping would make
rendering wait for preparation again. The retention regression prewarms common
font selections; pixel stability alone does not prove race freedom. A focused
future check should vary cold families/styles across concurrent shaping contexts.

### A2 — P1: retired GPU destruction races the successor's queue submissions

Locations: [BackgroundTransport.cpp:1531](../../src/worker/session/BackgroundTransport.cpp#L1531),
[PlaybackPipeline.cpp:469](../../src/worker/playback/PlaybackPipeline.cpp#L469),
[YuvGpu.cpp:862](../../src/worker/render/YuvGpu.cpp#L862),
[YuvGpu.cpp:507](../../src/worker/render/YuvGpu.cpp#L507).
Owner: **6.22b**.

`reapRetirement()` closes the retired pipeline on the render thread while its
successor can already be uploading. That close reaches `GpuYuvUploader::destroy()`
and `vkDeviceWaitIdle()`. It is outside `graphicsQueueMutex()`, whereas upload
submission holds that mutex. Joining the retired producer only silences that
producer, not the successor that now uses the same device queues.

This is an external host-synchronization violation: the installed Khronos registry
(`/usr/share/vulkan/registry/vk.xml`, command `vkDeviceWaitIdle`,
`implicitexternsyncparams`) requires synchronization of all ordinary queues
created from the device. VulkanContext creates ordinary queues, not queues with
the optional internally synchronized creation flag. Concurrent submission and
device-idle waiting are therefore not ordered by the current guard. Keeping
Ganesh destruction on its owner thread is necessary but insufficient.

Retire resources against their own completion fences/semaphores and synchronize
every queue operation that overlaps reclamation. If a global device wait remains
temporarily, it must participate in the same queue synchronization; it also
remains a render-thread wait. The latter's broader timeout/device-loss work is
already M7.1b, but this newly exposed concurrency is a 6.21c2 correctness issue.

### A3 — P1: retired background audio keeps playing after a swap or clear

Location: [BackgroundTransport.cpp:1483](../../src/worker/session/BackgroundTransport.cpp#L1483),
especially the retirement worker at line 1504. Owner: **6.22c**.

Teardown stops the video pipeline and detaches it from its AvPlayer, but never
holds, mutes or deactivates the old audio sink. The retirement thread first joins
the video pipeline and only then destroys the audio player. Throughout a slow
join the old player's feeder, mixer and active PipeWire sink can continue playing.

A new background can release its staged audio immediately, overlapping the old
clip. Clearing to a still or black can also leave the previous background's audio
audible after status has removed that transport. If the video join stalls, the
old audio lifetime is extended by the stall. Clock lifetime retention is correct;
continuing audible playback is not required to keep that clock object alive.

Separate audible deactivation from object destruction. The handoff must establish
the outgoing audio's intended state without waiting for the old producer to join;
the retained clock/player objects can then outlive that deactivation safely.
Future verification needs an audio-bearing old clip, delayed retirement, and a
swap/clear while capturing actual old/new source delivery. The current retirement
fixture checks frame counters and RPC timing, not outgoing audio delivery.

### A4 — P1: a second retirement makes the frame loop wait indefinitely

Location: [BackgroundTransport.cpp:1477](../../src/worker/session/BackgroundTransport.cpp#L1477).
Owner: **6.22d**.

The retirement slot is one deep. If A is still retiring after A→B, presenting C
(or clearing B) enters a sleep loop on the render thread until A's lane finishes.
This has no timeout and runs before B's own stop. A slow file, audio teardown or
driver call can therefore freeze audience updates on the second ordinary operator
action. Calling it a hung-producer overlap does not make the input sequence
exceptional: two quick changes naturally overlap any slow retirement.

Bounding the slot is appropriate; blocking its realtime consumer is the wrong
overflow policy. Keep the latest requested transition pending or explicitly
refuse/defer it while continuing to draw the current output. Do not replace this
with an unbounded retirement queue. Preserve the distinction between requested
and presented scene state while waiting for capacity. A future focused check
must issue A→B→C and A→B→clear before A's delayed retirement completes; the existing
fixture waits through the retirement after only one switch.

### A5 — P2: remaining synchronous media work contradicts scheduling closure

Locations: [BackgroundTransport.cpp:541](../../src/worker/session/BackgroundTransport.cpp#L541),
[BackgroundTransport.cpp:688](../../src/worker/session/BackgroundTransport.cpp#L688),
[BackgroundTransport.cpp:745](../../src/worker/session/BackgroundTransport.cpp#L745),
[BackgroundTransport.cpp:993](../../src/worker/session/BackgroundTransport.cpp#L993),
[AvPlayer.cpp:383](../../src/worker/playback/AvPlayer.cpp#L383).
Owner: **6.22e**; split into small leaves before implementation.

Three normal routes still wait on their caller:

- `media.standalone` synchronously calls `probeMedia()` on the RPC thread before
  it answers. A cold or stalled file delays every other RPC request.
- Publishing a staged standalone player probes the file again for duration on
  the render thread **under `fMutex`**. Both audience frames and status readers
  can wait for it. That metadata should travel with the staging result.
- Replacing or abandoning a standalone player destroys its AvPlayer on the render
  thread. The destructor joins feeders/mixer/PipeWire; the code has no contract
  proving these are always millisecond operations. Embedded-audio seek also waits
  up to 500 ms per feeder on this thread.

Move file discovery, waiting for seek completion and standalone stop/join work to
their owning control executor; publish immutable results or generation-tagged
completion events to rendering. Retain meaningful error acknowledgements, using
deferred replies where validation must complete before answering. Measure slow
probe, seek and stop separately: delaying only `AvPlayer::start()` cannot detect
these routes. The source comment in `applyTransportOps()` already says the seek
wait was meant to leave the loop; that move has not happened.

### A6 — P2: standalone status continues to describe a player already removed

Locations: [BackgroundTransport.cpp:723](../../src/worker/session/BackgroundTransport.cpp#L723),
[BackgroundTransport.cpp:792](../../src/worker/session/BackgroundTransport.cpp#L792).
Owner: **6.22f**.

When replacing an active track with a nonempty path, the render thread removes
the old AvPlayer, clears `fStandaloneLive` and stops it. However, the mirrored
`fMedia.standalone.live`, `playing`, `path`, position and lane are reset only for
an empty-path removal. `refreshStandaloneMediaLocked()` then returns early because
`fStandaloneLive` is false. Through the successor's entire slow stage, status
keeps reporting the old track as live/playing even though that player is gone.

The UI reads that mirror, while gain/mute validation reads the atomic gate, so
controls can appear available and be refused as "no standalone track is playing".
This is a missing state transition, not a sampling delay. Update player ownership,
command availability and the published mirror together. Define an explicit pending
selection state, or retain the old player until replacement is ready; do not
report the removed instance as current. Verify the intermediate status during a
slow replacement, not only the eventual new path and moving clock.

### A7 — P2: thumbnail completion can install old-sized pixels into a new request

Locations: [PreviewProducer.cpp:338](../../src/worker/session/PreviewProducer.cpp#L338),
[PreviewProducer.cpp:502](../../src/worker/session/PreviewProducer.cpp#L502),
[PreviewProducer.cpp:620](../../src/worker/session/PreviewProducer.cpp#L620).
Owner: **6.22g**.

The lane captures the requested width/height for decoding but publishes only a
path and image. When a newer thumbnail batch changes extent, request handling
correctly creates a fresh renderer/cache, yet the old completion unconditionally
installs its image into whichever renderer is current and settles matching paths
in the new batch. A 160×90 decode in flight can therefore satisfy a new 512×288
request without a new decode. The reverse direction also lets a small-extent cache
inherit the old request's larger allocation.

Path identity alone is insufficient for derived pixels. Carry stream/request or
cache-generation identity and decode extent with the completion, then accept it
only for a compatible destination. Reuse across batches can remain valid when
those properties match; a size change must not inherit an incompatible result.
Check extent replacement and stop/start during a delayed decode. The existing
slow-decode checks establish RPC responsiveness, not completion compatibility.

## Design direction and next verification

Keep the frontend/worker, document/scene and compositor boundaries. The problem
is concentrated in transport handoff and mutable cross-thread resources, not a
reason to rewrite the application or introduce a general job framework.

Give the media control path an explicit prepare → ready → commit → retire
lifecycle. Preparation produces a retained bundle with metadata and held sources;
rendering commits a ready bundle without file work, joins or capacity waits;
retirement separates audible deactivation, producer completion and GPU release.
Bounded capacity needs a deferred/coalesced policy and honest pending state.
Every asynchronous derived result needs enough identity to prove its destination.

Address A1–A4 first, then A5–A7, adding the focused regressions described above
with the fixes. After that, rerun existing suites and proceed to the M7 hardware,
failure-injection and long-service acceptance work. Historical green timing tests
and source comments are not substitutes for the missing ownership contracts.
