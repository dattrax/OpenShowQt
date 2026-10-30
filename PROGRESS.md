# OpenShowQt progress and handoff

Updated: 2026-10-30 (Europe/London). Reviewed code: `8402e69` (6.21g); **6.22a**
has landed since.

## Current checkpoint

The [architecture re-review](docs/reviews/M6-architecture-follow-up-2026-10-30.md)
found seven remaining issues. **6.22a (A1) is DONE**; **6.22b–g** remain in
[PLAN.md](PLAN.md) before claiming full conformance or advancing to M7. Preserve
the recorded 6.21 DONE checkpoints.

## Review summary

Three P1 issues remain: GPU queue synchronization during retired reclamation
(A2, **6.22b**); old audio continuing through retirement (A3, **6.22c**); and the
render thread waiting when a second retirement overlaps (A4, **6.22d**). Three P2
issues concern remaining synchronous media operations (A5, **6.22e**), stale
standalone status (A6, **6.22f**) and thumbnail completion compatibility (A7,
**6.22g**). Source locations and proposed contracts are in the review.

The fourth P1 — shared mutable font resolution across shaping threads — is
closed. `SceneTextEngine` builds no shared `FontCollection`: `fontCollection()`
creates one per threading context over the engine's shared `SkFontMgr`, so the
frame loop's cache miss, the prepare executor's prewarm and a thumbnail
companion never resolve cold families inside one unlocked table, and no render
path waits on preparation.

## 6.22a investigation record

`scene_render_tests`' `testConcurrentShapingOwnership` pins four things: stable
per-thread collection identity, six contexts resolving cold families
concurrently through one shared layout cache (complete and inked), the
prewarm→frame hand-off (a shaped-elsewhere handle paints this thread's own
pixels), and cross-collection pixel determinism.

Getting there changed the design's documentation. The fixture's first attempt
lost ink and intermittently crashed with SIGSEGV; both were the fixture's own
fault. Its canvas `scale()` compounded every round (restored now), and two
threads painted ONE shared handle. An isolated probe of that single shape —
six threads, one `SceneLayout`, otherwise nothing shared — crashes 4 runs in 5,
and the pinned source says why: `skparagraph` completes a line's text blobs
during that line's first paint (`modules/skparagraph/src/TextLine.cpp`,
`buildTextBlob` appends to `fTextBlobCache`). A laid-out Paragraph is therefore
not frozen: a handle may cross threads by hand-off, but exactly one thread may
paint it at a time. Production already honours that by ownership — a renderer
has one painter, its frame loop, and a thumbnail companion is a renderer of its
own (R16) — and `SceneText.h` now states the rule beside `drawLayout()` and the
`LayoutCache` contract.

## Verification and retained evidence

This leaf: `scene_render_tests` passes 20 consecutive runs (eight plain, twelve
under `MALLOC_PERTURB_`), and a `valgrind --tool=memcheck` pass over the binary
reports no invalid accesses. Gates: desktop **55/55** (283.18 s, 14
display-labelled), headless **32/32** (51.93 s); both trees build clean.

Historical 55/55 desktop and 32/32 headless results and 6.21 completion details
are preserved in [history](docs/history/PROGRESS-through-6.21g.md).

## Next action

**6.22b**: order retired GPU reclamation against successor submissions —
`reapRetirement()` reaching `GpuYuvUploader::destroy()` / `vkDeviceWaitIdle`
(`YuvGpu.cpp`) must participate in the queue's synchronization or retire against
its own completion fences. Then **6.22c–d**, then **6.22e–g**; split **6.22e**
into small leaves before starting it. M7 recovery, budgets, packaging and
hardware/long-service acceptance remain TODO.
