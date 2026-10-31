# OpenShowQt progress and handoff

Updated: 2026-10-30 (Europe/London). Reviewed code: `8402e69` (6.21g); **6.22a**,
**6.22b** and **6.22c** have landed since.

## Current checkpoint

The [architecture re-review](docs/reviews/M6-architecture-follow-up-2026-10-30.md)
found seven remaining issues. **6.22a (A1)**, **6.22b (A2)** and **6.22c (A3)**
are DONE; **6.22d–g** remain in [PLAN.md](PLAN.md) before claiming full
conformance or advancing to M7. Preserve the recorded 6.21 DONE checkpoints.

## Review summary

One P1 remains: the render thread waits when a second retirement overlaps
(A4, **6.22d**). Three P2 issues concern remaining synchronous media
operations (A5, **6.22e**), stale standalone status (A6, **6.22f**) and
thumbnail completion compatibility (A7, **6.22g**). Source locations and
proposed contracts are in the review.

Three are closed. Shared mutable font resolution: `SceneTextEngine` builds no
shared `FontCollection`; `fontCollection()` creates one per threading context
over the engine's shared `SkFontMgr`. Queue-synchronized reclamation: all three
whole-device teardowns hold `graphicsQueueMutex()` around `vkDeviceWaitIdle`.
Audible retirement: the teardown hand-off now calls `pause()` on the outgoing
`AvPlayer` — sink stream deactivated, clock latched, no thread joined — before
the retirement lane exists, so what the lane's join-then-destroy extends is the
player's object lifetime only, never its audibility; a successor's staged audio
and a clear-to-still no longer overlap a retired clip.

## Leaf records

**6.22c.** The contract "separate audible deactivation from object
destruction" needed verification the review itself said was missing: an
audio-bearing old clip, a delayed retirement, and a capture of actual
old-source delivery. `BackgroundTransport` gained one narrow bench seam,
`retiringAudioDelivery()` (paused / sinkActive / deliveredFrames of the
in-flight retirement's player) — the status mirror cannot tell
silent-because-handed-off from silent-because-destroyed, only the lane's own
slot can. Making the seam safe moved the `fRetirement` pointer itself under
`fMutex` (the lane moves the player out under the same lock before stopping it
outside), and `background_transport_tests` — previously decision-only — gained
its first real-device fixture: audio-bearing ffmpeg clip, real PipeWire sink,
headless Vulkan, 6.21c2's scripted 700 ms pacing exit. It asserts the clear
lands in milliseconds, the retired player is already paused with its stream
inactive, and its delivered frames are frozen across 200 ms of the scripted
exit — then that the frame-loop's own `applyTransportOps()` pass reaps the
lane. Removing the `pause()` fails both audio checks deterministically
(negative control verified). The fixture skips cleanly with whatever of
ffmpeg/daemon/Vulkan is missing, and ran fully in both presets.

**6.22b.** Two fixtures in `gpu_decode_tests`.
`reclamationWaitsBehindTheQueueGuard` holds the queue guard the way a frame
holds it and shows `destroy()` waits for it; `overlappingRetirementKeepsThe
SuccessorUp` runs twelve create/destroy passes beside a live committing
producer. Vulkan validation does **not** flag the pre-fix device-idle/submit
overlap — host-side external-sync violations of that class are invisible to
the layer — so the deterministic ordering fixture is the regression that
matters. The review's per-slot completion-fence alternative is deferred to
M7.1b (timeouts, device loss), recorded as deferred, not dismissed.

**6.22a.** `scene_render_tests`' `testConcurrentShapingOwnership` pins stable
per-thread collection identity, six contexts resolving cold families
concurrently through one shared layout cache (complete and inked), the
prewarm→frame hand-off, and cross-collection pixel determinism. Writing it
surfaced the finding's second half: `skparagraph` completes a line's text blobs
during that line's first paint (`modules/skparagraph/src/TextLine.cpp`), so a
laid-out Paragraph is not frozen — a handle crosses threads by hand-off but
exactly one thread may paint it at a time (an isolated probe of two threads
painting one handle crashes 4 runs in 5). Production keeps that by ownership,
one frame loop per renderer; `SceneText.h` states the rule.

## Verification and retained evidence

6.22c: the new fixture passes three consecutive runs; the negative control
(pause removed) fails exactly the two A3 checks. Gates: desktop **55/55**
(285.73 s, 14 display-labelled), headless **32/32** (55.16 s); both trees
build clean. Fixture evidence line: hand-off 0.1–0.2 ms, retired delivery
12288 frames frozen across the window of the scripted 700 ms exit.

6.22b: `gpu_decode_tests` five runs plain and with validation on; desktop
55/55 (283.86 s), headless 32/32 (53.59 s).

6.22a: `scene_render_tests` 20 consecutive runs, `valgrind --tool=memcheck`
clean; desktop 55/55 (283.18 s), headless 32/32 (51.93 s).

Historical 55/55 desktop and 32/32 headless results and 6.21 completion details
are preserved in [history](docs/history/PROGRESS-through-6.21g.md).

## Next action

**6.22d**: replace the slot-full sleep loop in `teardownBackgroundTransport`
(the wait on a prior retirement's `laneDone`) with a deferred/coalesced
latest-transition policy, so a second swap during a delayed retirement does not
block the frame loop; fixtures for A→B→C and A→B→clear while A's retirement is
delayed. Then **6.22e–g**; split **6.22e** into small leaves before starting
it. M7 recovery, budgets, packaging and hardware/long-service acceptance remain
TODO.
