# OpenShowQt progress and handoff

Updated: 2026-10-30 (Europe/London). Reviewed code: `8402e69` (6.21g); **6.22a**
and **6.22b** have landed since.

## Current checkpoint

The [architecture re-review](docs/reviews/M6-architecture-follow-up-2026-10-30.md)
found seven remaining issues. **6.22a (A1)** and **6.22b (A2)** are DONE;
**6.22c–g** remain in [PLAN.md](PLAN.md) before claiming full conformance or
advancing to M7. Preserve the recorded 6.21 DONE checkpoints.

## Review summary

Two P1 issues remain: old audio continuing through retirement (A3, **6.22c**)
and the render thread waiting when a second retirement overlaps (A4, **6.22d**).
Three P2 issues concern remaining synchronous media operations (A5, **6.22e**),
stale standalone status (A6, **6.22f**) and thumbnail completion compatibility
(A7, **6.22g**). Source locations and proposed contracts are in the review.

Two P1s are closed. Shared mutable font resolution: `SceneTextEngine` builds no
shared `FontCollection`, `fontCollection()` creates one per threading context
over the engine's shared `SkFontMgr`. Queue-synchronized reclamation: all three
whole-device teardowns (`GpuYuvUploader::destroy`, `Swapchain::destroy`,
`VulkanContext::destroy`) now hold `graphicsQueueMutex()` around
`vkDeviceWaitIdle`, so a retired transport's teardown is ordered against a
successor's guarded `commitSlot` — the external-synchronization obligation the
installed registry attaches to that call is now met by the operation, not by
hope at the call sites. The review's alternative (per-slot retirement against
completion fences) was deferred deliberately: the wait stays whole-device, and
its timeout/device-loss behaviour belongs to M7.1b, which owns that machinery.

## Leaf records

**6.22b.** Two fixtures in `gpu_decode_tests`.
`reclamationWaitsBehindTheQueueGuard` is the ordering proof with teeth: hold
the queue guard the way a frame holds it, start a `destroy()`, show it has not
completed, release, show it does — a negative control with the guard removed
fails this deterministically. `overlappingRetirementKeepsTheSuccessorUp` runs
twelve create/destroy passes beside a live producer that polls and releases
like an audience, asserting each reclaim clears the guard within two seconds
and the producer ends with no fatal error. One honesty note for future
verification work: Vulkan validation (enabled via `OPENSHOW_VULKAN_VALIDATION=1`)
does **not** flag the pre-fix device-idle/submit overlap — the layer's queue
tracking cannot see host-side external-sync violations of this class, so the
deterministic ordering fixture, not VVL, is the regression that matters.

**6.22a.** `scene_render_tests`' `testConcurrentShapingOwnership` pins stable
per-thread collection identity, six contexts resolving cold families
concurrently through one shared layout cache (complete and inked), the
prewarm→frame hand-off, and cross-collection pixel determinism. Writing it
surfaced the finding's second half: `skparagraph` completes a line's text blobs
during that line's first paint (`modules/skparagraph/src/TextLine.cpp`,
`buildTextBlob` appends to `fTextBlobCache`), so a laid-out Paragraph is not
frozen — a handle crosses threads by hand-off but exactly one thread may paint
it at a time (an isolated probe of two threads painting one handle crashes 4
runs in 5). Production keeps that by ownership, one frame loop per renderer;
`SceneText.h` states the rule beside `drawLayout()` and the `LayoutCache`.

## Verification and retained evidence

6.22b: `gpu_decode_tests` passes five consecutive runs plain and with
validation on; the negative control (guard removed) fails the ordering check
deterministically. Gates: desktop **55/55** (283.86 s, 14 display-labelled),
headless **32/32** (53.59 s); both trees build clean.

6.22a: `scene_render_tests` passed 20 consecutive runs (eight plain, twelve
under `MALLOC_PERTURB_`) and a `valgrind --tool=memcheck` pass reported no
invalid accesses. Gates then: desktop 55/55 (283.18 s), headless 32/32 (51.93 s).

Historical 55/55 desktop and 32/32 headless results and 6.21 completion details
are preserved in [history](docs/history/PROGRESS-through-6.21g.md).

## Next action

**6.22c**: deactivate the outgoing background audio before (and without
waiting for) the video join — `teardownBackgroundTransport` in
`BackgroundTransport.cpp` must stop audio audibility at hand-off while the
clock/player objects stay alive for the retirement lane; verify no old audio
after swap and after clear. Then **6.22d** (replace the slot-full sleep loop
with a latest-transition policy), then **6.22e–g**; split **6.22e** into small
leaves before starting it. M7 recovery, budgets, packaging and hardware/long-
service acceptance remain TODO.
