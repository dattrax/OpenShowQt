# OpenShowQt progress and handoff

Updated: 2026-10-30 (Europe/London). Reviewed code: `8402e69` (6.21g); **6.22a**
through **6.22d** and **6.22e1–e3** have landed since.

## Current checkpoint

The [architecture re-review](docs/reviews/M6-architecture-follow-up-2026-10-30.md)
found seven remaining issues. **6.22a (A1)**, **6.22b (A2)**, **6.22c (A3)**
and **6.22d (A4)** are DONE — all four P1s are closed — and **6.22e** is in
progress with its recorded split: **6.22e1–e3 DONE**, **6.22e4** and
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

**6.22e3.** A replaced or abandoned standalone player was destroyed on the
render thread — `~AvPlayer` joins the feeders, mixer and PipeWire sink,
and the review rightly called "normally milliseconds" an unproven
contract. The track now has its OWN one-deep retirement lane (deliberately
not shared with the background transport's: a hung audio server must not
defer the audience's pictures, nor vice versa). The hand-off pauses the
outgoing player — silent here, 6.22c's rule for the other player — and the
lane runs `stop()` while the player stays visible in the slot, so the new
`retiringStandaloneDelivery()` seam can watch the whole wait (its three
reads are atomics by construction; the object outlives them via the slot).
Capacity mirrors 6.22d: a swap arriving behind a busy lane is DEFERRED —
the latest intent back into `fStandaloneSwap` unless something newer
already rode it — and the lane's wake brings the retry; a superseded
staged player parks in its (held, silent) stage rather than dying inline;
a silent placeholder never occupies the lane so its swap never defers;
shutdown drains the lane with the other waits. New fault seam
`OPENSHOW_FAULT_PLAYER_STOP_MS/_MATCH` sleeps inside `stop()` after the
exits open — the stalled-server shape — and pays exactly once per player
(the reap's destructor re-enters `stop()`; the joins it would re-simulate
already ran). The fixture scripts A's death at 700 ms: the swap pass takes
1.4 ms, the seam shows paused-and-frozen immediately, B publishes beside
the dying lane, and a removal inside the window defers at 0.1 ms with the
mirror still honestly naming live B, landing on pass 27 of the 20 ms pump
(≈ the scripted window — capacity-bound, not lucky). Negative controls:
inline destruction fails the timing and both seam checks at 708 ms; no
deferral pays the lane inside the removal pass (536 ms).

**6.22e2.** `media.standalone` was the last RPC verb that opened a file on
the RPC thread before answering — one cold track delayed status, display
changes, release and shutdown behind it. The transport split into
`validateStandaloneTrack()` (probe + verdict, const, carries the
`OPENSHOW_FAULT_STANDALONE_PROBE` seam) and `armStandaloneTrack()` (the
newest-wins slot write, no probe); the sync `requestStandaloneAudio()`
composition stays for the bench. `OutputSession::requestStandaloneAudioDeferred`
submits the verdict to `fPrepares` under the request's id — the executor's
one-thread FIFO keeps both the reply order and the arm order correct with a
single swap slot — and the RPC handler answers through `fDefer`, the
`scene.prepare` route: removals and no-media sessions answer inline, a job
cancelled before it ran answers as a refusal naming the retirement, and a
submit refusal (stopped/full) answers `kInternalError` synchronously
because nothing exists yet to answer later. No supersede machinery: two
selections in flight arm newest-last and answer in id order. The socket
fixture sends `media.standalone` raw and measures four status round trips
across the 600 ms scripted probe window (0.2–0.8 ms each), then awaits the
deferred reply (612 ms — the verdict rode its probe), refuses a missing
file with its probe's own reason after the window while the live track
stays live, and takes the removal inline. The test client gained
`sendRequest`/`awaitReply` plus a stash in `call()` (a deferred reply can
surface inside another request's poll; discarding it would strand the
verdict). Reverting the handler to the synchronous call (negative control)
fails exactly the two responsiveness checks with a 608 ms status RTT.

**6.22e1.** The standalone publish re-opened the container for its duration
on the render thread inside `fMutex`; the bring-up lane now probes once and
the staged value rides `StandaloneStage::duration` (release/acquire with the
stage's `done` flag) into the status mirror. New fault seam
`OPENSHOW_FAULT_STANDALONE_PROBE_MS/_MATCH` sits with the file read in
`BackgroundTransport::probeStandaloneDuration`, so the fixture measures
WHERE the probe is paid: with 500 ms scripted, the worst transport pass and
a concurrent `mediaStatus()` reader measured 0 ms; restoring the inline
probe under the lock (negative control) measured 516 ms on both — the exact
A5 symptom.

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

6.22e3: the new fixture passed five consecutive runs, identically
(hand-off 1.4–1.7 ms, delivery frozen at 12288 frames, removal deferred
0.0–0.1 ms and landed on pass 27 — evidence lines in both presets' logs);
negative controls fail exactly the A5-route checks (inline destruction:
708.2 ms swap pass, seam absent; deferral removed: 536 ms removal pass).
Gates: desktop **55/55** (291.13 s, 14 display-labelled), headless
**32/32** (58.47 s); both trees build clean.

6.22e2: the new scenario passes; the negative control (handler reverted to
the synchronous probe) fails exactly the two status-responsiveness checks
(608 ms worst RTT against the 600 ms seam). Gates: desktop **55/55**
(292.46 s, 14 display-labelled), headless **32/32** (57.66 s); both trees
build clean. Fixture evidence lines (desktop; the scenario is
display-labelled and spawns a real worker): deferred accept 612–613 ms
against a 600 ms scripted probe, worst status RTT through the window
0.2–0.8 ms, refusal answered 606–611 ms after its probe window.

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

**6.22e4**: move the seek-completion wait off the render thread.
`AvPlayer::seek` (src/worker/playback/AvPlayer.cpp ~383) fans the seek out
then `waitSeekApplied(500)`s per feeder — on the transport pass — so a
slow feeder acknowledgment holds the frame loop up to half a second per
input. The shape: begin the seek on the render thread, wait and flush on
a control-lane completion tagged with the transport generation, hold
later transport intents in their existing slots while a seek runs, let
retirement join the seek worker before stopping the player, and add a
slow-ack fault seam so the fixture measures the wait wherever it is paid.
Then **6.22f** (standalone ownership pending/live mirror,
`BackgroundTransport.cpp` ~723/792) and **6.22g** (thumbnail completion
carries generation/extent, `PreviewProducer.cpp` ~338/502/620). When a–g
are DONE: align the PLAN header and this handoff with the re-review
evidence. M7 recovery, budgets, packaging and hardware/long-service
acceptance remain TODO.
