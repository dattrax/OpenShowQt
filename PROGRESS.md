# OpenShowQt progress and handoff

Updated: 2026-10-30 (Europe/London). Reviewed code: `8402e69` (6.21g); **6.22a**
through **6.22d** and **6.22e1** have landed since.

## Current checkpoint

The [architecture re-review](docs/reviews/M6-architecture-follow-up-2026-10-30.md)
found seven remaining issues. **6.22a (A1)**, **6.22b (A2)**, **6.22c (A3)**
and **6.22d (A4)** are DONE — all four P1s are closed — and **6.22e** is in
progress with its recorded split: **6.22e1 DONE**, **6.22e2–e4** and
**6.22f–g** remain in [PLAN.md](PLAN.md) before claiming full conformance or
advancing to M7. Preserve the recorded 6.21 DONE checkpoints.

## Review summary

Three P2 issues remain: remaining synchronous media operations (A5, **6.22e**),
stale standalone status (A6, **6.22f**) and thumbnail completion compatibility
(A7, **6.22g**). Source locations and proposed contracts are in the review.

The P1s are closed. Shared mutable font resolution: `SceneTextEngine` builds
no shared `FontCollection`; one per threading context over the shared
`SkFontMgr`. Queue-synchronized reclamation: all three whole-device teardowns
hold `graphicsQueueMutex()` around `vkDeviceWaitIdle`. Audible retirement: the
hand-off calls `pause()` on the outgoing `AvPlayer` before the lane exists —
the join extends object lifetime, never audibility. Deferral under capacity:
the one-deep retirement slot no longer blocks the frame loop —
`applyBackground()` with a busy slot stores the latest wanted transition and
returns in microseconds; the next `applyTransportOps()` pass (which the lane's
wake makes prompt) reaps and retries it, and a started retry makes the loop
owe a present exactly like the scene-driven call did.

## Leaf records

**6.22e1.** The standalone publish re-opened the container for its duration
on the render thread inside `fMutex`; the bring-up lane now probes once and
the staged value rides `StandaloneStage::duration` (release/acquire with the
stage's `done` flag) into the status mirror. New fault seam
`OPENSHOW_FAULT_STANDALONE_PROBE_MS/_MATCH` sits with the file read in
`BackgroundTransport::probeStandaloneDuration`, so the fixture measures
WHERE the probe is paid: with 500 ms scripted, the worst transport pass and
a concurrent `mediaStatus()` reader measured 0 ms; restoring the inline
probe under the lock (negative control) measured 516 ms on both — the exact
A5 symptom. **6.22e2** is next (probe the RPC-thread `media.standalone`
validation via the prepare executor and a deferred reply), then **e3**
(standalone stop on a lane) and **e4** (seek-completion wait off the loop).

**6.22d.** The old overflow policy was an untimed sleep at the head of
`teardownBackgroundTransport` — the second ordinary operator change behind a
slow exit froze audience updates with no timeout. The replacement is a
policy, not a wait: `PendingTransition` (wanted background + GPU handles +
resident closure + timestamp, render-thread-only member) holds the LATEST
deferred ask; a burst of changes coalesces into it; `sameIdentity` against
the still-on-screen background cancels it (a return outranks the deferred
ask); a swap of a still never defers (only a video teardown ever occupies
the slot); and there is deliberately no queue — the scene store keeps
everything older. The teardown's sleep loop became a precondition check with
a BUG log (its drain stays as the only total response — overwriting the slot
would destroy a joinable `std::thread`); shutdown now drains BEFORE its
teardown (precondition) and after (its own hand-off must not outlive the
object). `applyTransportOps()` returns whether a retry STARTED a transport
and the session loop turns that into `fNeedsPresent`. Two scenarios in
`background_transport_tests` drive it with the scripted 800 ms pacing exit:
A→B→C and A→B→clear, each asserting the third call returns in <100 ms with
`started == false`, the mirror still honestly reports the live B, and the
transition lands on a later `applyTransportOps()` pass (~pass 40 ≈ the
scripted window — the retry is provably capacity-bound, not lucky).
Disabling the deferral branch fails exactly those checks and prints the BUG
line (negative control verified).

**6.22c.** The contract "separate audible deactivation from object
destruction" needed the review's missing verification: an audio-bearing old
clip, a delayed retirement, and a capture of actual old-source delivery. One
narrow bench seam, `retiringAudioDelivery()` (paused / sinkActive /
deliveredFrames of the in-flight retirement's player) — the status mirror
cannot tell silent-because-handed-off from silent-because-destroyed. The
`fRetirement` pointer rides `fMutex`; the lane moves the player out under the
same lock before stopping it outside. `background_transport_tests` gained its
first real-device fixture (audio-bearing ffmpeg clip, real PipeWire sink,
headless Vulkan, scripted 700 ms exit): clear lands in milliseconds, the
retired player is already paused, delivery frozen across the window; removing
the `pause()` fails both checks deterministically.

**6.22b.** Two fixtures in `gpu_decode_tests`: `reclamationWaitsBehindThe
QueueGuard` (deterministic ordering proof; negative control fails every run)
and `overlappingRetirementKeepsTheSuccessorUp` (twelve overlapped reclaims
beside a committing producer). Vulkan validation does NOT flag the pre-fix
overlap — the ordering fixture is the regression that matters. Per-slot
completion fences deferred to M7.1b, recorded as deferred, not dismissed.

**6.22a.** `scene_render_tests`' `testConcurrentShapingOwnership`: per-thread
collections, six contexts resolving cold families through one shared layout
cache, prewarm→frame hand-off, cross-collection pixel determinism. Surfaced
the finding's second half: `skparagraph` completes a line's text blobs during
that line's first paint, so a laid-out Paragraph is not frozen — a handle
crosses threads by hand-off but exactly one thread may paint it at a time
(`SceneText.h` states the rule).

## Verification and retained evidence

6.22e1: the new fixture passes; the negative control (inline probe restored
under `fMutex`) fails both timing checks at ~516 ms against the scripted
500 ms. Gates: desktop **55/55** (288.45 s, 14 display-labelled), headless
**32/32** (57.72 s); both trees build clean. Fixture evidence line:
scripted 500 ms probe, worst transport pass 0.0–0.1 ms, worst status read
0.0 ms, duration 2.000 s — ran in both presets.

6.22d: the new fixture passes three consecutive runs; the negative control
(deferral disabled) fails exactly the A4 checks and prints the BUG invariant
line. Gates: desktop **55/55** (286.88 s, 14 display-labelled), headless
**32/32** (57.17 s); both trees build clean. Fixture evidence lines:
A→B→C deferred 0.0 ms, landed on pass 40–41; A→B→clear deferred 0.0 ms,
landed on pass 40 — ran fully in both presets (not skipped).

6.22c: three clean runs; negative control fails exactly the two A3 checks;
desktop 55/55 (285.73 s), headless 32/32 (55.16 s). Hand-off 0.1–0.2 ms,
retired delivery 12288 frames frozen across the scripted 700 ms exit.

6.22b: five runs plain and with validation on; desktop 55/55 (283.86 s),
headless 32/32 (53.59 s). 6.22a: 20 runs, `valgrind` clean; desktop 55/55
(283.18 s), headless 32/32 (51.93 s).

Historical 55/55 desktop and 32/32 headless results and 6.21 completion details
are preserved in [history](docs/history/PROGRESS-through-6.21g.md).

## Next action

**6.22e2**: move the `media.standalone` validation probe off the RPC thread
— schedule it on the session's prepare executor and answer with a deferred
reply (the `scene.prepare` pattern and its `fDefer` bag), keeping accept,
refuse and removal semantics; a socket fixture with the probe seam armed
must show status answering while a slow standalone probe runs. Then
**6.22e3** (the replaced standalone player's stop/join on a lane, silent at
hand-off like 6.22c) and **6.22e4** (the seek-completion wait off the render
thread behind a generation-tagged completion). Then **6.22f** (standalone
ownership pending/live mirror, `BackgroundTransport.cpp` ~723/792) and
**6.22g** (thumbnail completion carries generation/extent,
`PreviewProducer.cpp` ~338/502/620). When a–g are DONE: align the PLAN header
and this handoff with the re-review evidence. M7 recovery, budgets, packaging
and hardware/long-service acceptance remain TODO.
