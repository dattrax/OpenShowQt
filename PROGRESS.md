# OpenShowQt progress and handoff

Updated: 2026-10-31 (Europe/London). Reviewed code: `8402e69` (6.21g); **6.22a**
through **6.22f** have landed since.

## Current checkpoint

The [architecture re-review](docs/reviews/M6-architecture-follow-up-2026-10-30.md)
found seven remaining issues. **6.22a (A1)**, **6.22b (A2)**, **6.22c (A3)**,
**6.22d (A4)**, **6.22e (A5, all four leaves)** and **6.22f (A6)** are DONE —
all four P1s are closed, and so are the blocking-media and the stale-status
findings — while **6.22g** remains in [PLAN.md](PLAN.md) before claiming full
conformance or advancing to M7. Preserve the recorded 6.21 DONE checkpoints.

## Review summary

One P2 issue remains: thumbnail completion compatibility (A7, **6.22g**).
Source locations and proposed contracts are in the review.

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
owe a present exactly like the scene-driven call did. And the standalone
mirror no longer outlives its player: the swap's claim publishes the pending
selection (new path, `live=false`, no position, replacement error cleared),
so status and the command gate agree throughout a slow replacement.

## Leaf records

**6.22f.** Replacing an ACTIVE standalone track removed the old player,
cleared the command gate and took the instance down, but only the empty-path
removal also updated the published block: `refreshStandaloneMediaLocked()`
early-returns while no player is up ("the swap owns the block"), and until
now the swap had said nothing at all for a replacement. So through the
successor's entire bring-up `media.standalone` reported the GONE player as
live and playing at the position where it froze, while `requestMediaGain`/
`requestMediaMute` were already refusing with "no standalone track is
playing" — controls that look alive and are refused. That is a missing state
transition, not a sampling delay. The claim now publishes the pending
selection for both cases: live=false, playing=false, position and duration
-1, lane "none", loops 0, and for a replacement the block also names the NEW
selection and drops the outgoing instance's sticky error (it belongs to the
player that just left; beside a new path it would tell a story this selection
never had, and a fresh failure writes its own). A removal keeps path and
error together — what broke, and where, is still the story the operator needs
and nothing new is being asked of the block. Requested-not-yet-presented is
then exactly what the gate refuses. The fixture brings A up to live, playing
and advancing, scripts
B's bring-up at 700 ms (`OPENSHOW_FAULT_AUDIO_START_MS/_MATCH`) so the
intermediate window dominates the swap, and reads the block on the claim pass
and again five passes later: not live, not playing, B's path, no position —
and the gain command refused with the same truth — before B comes live and
advancing on pass 32 of the 20 ms pump. A second leg pins the error's owner:
with the mixer fault set, a selection's bring-up fails and writes its own path
and sticky error (pass 2), and the next claim names its new path with the
error gone. The negative control (the field reset back under the
`swap->empty()` guard) reports `live=1 playing=1` at a frozen 0.06 s beside
the OLD path and keeps the dead selection's error beside the new one: the
finding's exact symptom, five checks.

**6.22e4.** The transport pass contained the seek's wait: `AvPlayer::seekTo`
fanned the jump out and then `waitSeekApplied(500)`-ed every feeder inline,
so a slow container acknowledgment held the audience's frame loop for the
whole window. The seek split into three halves — `beginSeekTo` (fan-out, no
waiting), `waitSeekTo` (the cost, abortible by the waiter) and
`finishSeekTo` (sink flush, clock rebase, fallback move) — with the sync
`seekTo` kept as their composition for callers that own their blocking (the
bench, `av_sync_tests`). On the transport, a seek is now a one-deep FLIGHT:
the pass fans out and spawns a worker that only waits, publishes its verdict
and wakes the loop; the completing pass runs the finish half and the
fault report on the transport's own thread. Ordering survives the split
because the momentary-op slot is not consumed while a flight runs — pause,
play and a newer seek hold their turn behind the seek, exactly the order
the synchronous code enforced — while gain, mute and loop (independent
slots) proceed. The flight is its own generation: no second can stack, and
it retires WITH its transport — teardown moves it into the Retirement,
sets its abort, and the lane joins the seek worker BEFORE the player's
stop, because the worker waits on feeders that player owns (a flight whose
transport died is discarded, never replayed); `retireAudioInstance` drains
one too, and shutdown's existing lane drains cover it. New fault seam
`OPENSHOW_FAULT_SEEK_ACK_MS/_MATCH` sleeps in the feeder's settle path
between the container's jump and its acknowledgment — the window a waiter
pays; it also gave `AudioFileFeeder` its long-vestigial `fPath` (now
recorded in `open()`). The fixture scripts a 300 ms ack: the seek pass
takes 0.1 ms, a pause asked for mid-flight is still unapplied (status says
playing) while the wait holds, and completion lands at pass ~15 of the
20 ms pump with the position at the target (2.33 s read for a 2.0 s seek —
the clock's honest lag) and the held pause
applied behind it; a swap across the second flight hand-offs at 0.1 ms and
shutdown drains the lane holding the waiting worker. Negative controls:
the synchronous route pays the ack on the pass (307.6 ms) and swallows the
pause's turn; consuming ops mid-flight fails exactly the hold check (and a
variant with no single-flight rule at all dies in the thread destructor —
the gate is what makes the precondition total).

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

6.22f: the new fixture passed five consecutive runs, identically (mid-
replacement status `live=0 playing=0 position=-1.00`, B published on pass 32
of the 20 ms pump after its scripted 700 ms start, a failed bring-up writing
its own path and error on pass 2 and the next claim naming the new path with
an empty error — evidence lines in both presets' logs); the negative control
(field reset restored to the empty-path branch only) fails exactly the five
A6 checks and prints `live=1 playing=1 position=0.06` mid-replacement, i.e.
the gone player reported as current, plus the stale error beside a new path.
`background_media_tests` passes unchanged (its removal and swap scenarios read
live/playing/position, which the pending state agrees with). Gates: desktop
**55/55** (293.03 s, 14 display-labelled), headless **32/32** (60.61 s); both
trees build clean.

6.22e4: the new fixture passed five consecutive runs, identically (seek
pass 0.1–0.2 ms against the 300 ms scripted ack, completion and held pause
at pass 15–16 at 2.33 s, mid-flight swap 0.1 ms — evidence lines in both
presets' logs); negative controls fail exactly the A5 checks (synchronous
route: 307.6 ms seek pass and the swallowed pause; op slot opened
mid-flight: the hold check alone). One earlier desktop gate run failed a
6.22e2 check (that scenario's worker answered `shutdown` later than the
harness's 5 s poll under ctest load); the scenario touches none of the
6.22e4 code paths, and the failure did not reproduce in three standalone
re-runs or in the re-run gate. Gates: desktop **55/55** (291.25 s, 14
display-labelled), headless **32/32** (59.05 s); both trees build clean.

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

**6.22g** (review A7, the last open finding): the thumbnail lane captures the
requested width/height for its decode but publishes only a path and an image,
and the completion is then adopted unconditionally — into whichever renderer
is current, settling matching paths in whatever batch is current too. So a
160×90 decode still in flight can satisfy a newer 512×288 request without a
new decode (and the reverse lets a small-extent cache inherit a larger
allocation). Carry stream/request or cache-generation identity and the decode
extent with the completion and accept it only for a compatible destination;
reuse across batches stays valid when those properties match. The review asks
for both races under a delayed decode — extent replaced, and stop/start of the
stream — noting the existing slow-decode checks establish RPC responsiveness,
not completion compatibility (`PreviewProducer.cpp` ~338/~502/~620). When that
lands a–g are all DONE: align the PLAN header and this handoff with the
re-review evidence and restate the conformance claim. M7 recovery, budgets,
packaging and hardware/long-service acceptance remain TODO.
