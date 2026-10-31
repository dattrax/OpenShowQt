# Review after architecture follow-up 6.22

Historical findings at `db406f7`. Their 6.23 fixes and final verification are
recorded in the [closure evidence](M6-6.23-verification-2026-10-31.md).

Reviewed 2026-10-31 (Europe/London), clean code commit `db406f7` (6.22g),
against the implementation since `afa63e9`. Production code and existing tests
are unchanged by this review. Five issues remain: four P1 and one P2.
Four were reproduced against the production worker; the retirement ownership
race is a source-level finding whose crash was not deliberately induced.

The specific fixes made in 6.22 are present, but their asynchronous operations
do not yet compose correctly at the session boundary. Keep the DONE checkpoints
and their evidence; **6.23a–e** track the remaining work before full conformance
can be claimed. Recovery, bounded device-loss handling, packaging, resource
budgets and physical/long-service acceptance remain the existing M7 work.

## Findings

### B1 — P1: a new selection can bypass the standalone retirement capacity check

Locations: [BackgroundTransport.cpp:708](../../src/worker/session/BackgroundTransport.cpp#L708),
[BackgroundTransport.cpp:720](../../src/worker/session/BackgroundTransport.cpp#L720),
[BackgroundTransport.cpp:972](../../src/worker/session/BackgroundTransport.cpp#L972).
Owner: **6.23a**. Evidence: source tracing; no crash reproduction.

`applyStandaloneOps()` reads whether a completed stage is superseded under
`fStandaloneSwapMutex`, releases the mutex, and checks retirement capacity only
if that first read said it was superseded. It then takes the stage, joins its
worker, and reads supersession again under a second lock. Those two reads need
not agree: the prepare executor can arm a newer selection between them.

An old standalone player can still be retiring while stage B has completed.
If the first read sees no newer selection, the busy retirement check is skipped.
Selection C can then arrive before the second read, which now abandons B and
calls `retireStandaloneInstance()`. That function unconditionally overwrites
`fStandaloneRetirement`, although the previous lane still owns a joinable thread.
When the old lane releases its last shared reference, destruction of that
joinable `std::thread` calls `std::terminate`. A later shutdown cannot join a
retirement whose owning slot was already overwritten.

Make supersession choice, capacity reservation and stage consumption one
consistent decision. Preserve the stage when capacity is unavailable. Enforce
the empty-slot precondition at the actual retirement handoff, rather than
assuming an earlier observation established it. Add a controlled interleaving
fixture that arms C between the two current checks while A's retirement is busy;
the existing fixture supplies supersession before the first check.

### B2 — P1: deferred background changes are credited as completely presented

Locations: [OutputSession.cpp:922](../../src/worker/session/OutputSession.cpp#L922),
[OutputSession.cpp:938](../../src/worker/session/OutputSession.cpp#L938),
[BackgroundTransport.cpp:1484](../../src/worker/session/BackgroundTransport.cpp#L1484),
[SceneStore.cpp:189](../../src/worker/session/SceneStore.cpp#L189).
Owner: **6.23b**. Evidence: live production-worker probe.

The transport now defers a background swap when retirement capacity is occupied,
but its caller has already landed the new scene and installed it in SceneRenderer.
The return value is only a boolean meaning "video started"; it cannot distinguish
deferred work from a successful still/clear/no-op. The next present therefore
commits the new scene revision while the old transport and held video remain.
SceneStore's pending state becomes zero even though a required part of that
scene has not changed.

The probe delayed A's retirement by three seconds, switched A→B, then presented
video C while A still occupied the retirement slot. In **20.6 ms**, status said
`scene.active=3`, `scene.pending=0`, while `media.path` still named B. The same
contradiction remained 200 ms later. For video C, the renderer receives B's held
video image with C's authored scene; this pixel consequence follows from source
tracing, while the measured proof here is the contradictory live status.

Deferred work must remain a pending *scene transaction*, with renderer content,
transport identity and acknowledged revision committed together. Return a typed
outcome such as applied/deferred/failed instead of conflating it with whether a
video started. Keep drawing the previously committed scene until the new scene
can land, unless an explicitly defined partial-presentation contract is adopted.
Extend the capacity test through OutputSession and the real frontend, asserting
scene pending/active, media identity and preview pixels during A→B→C and clear.
The new component fixture exercises BackgroundTransport without those owners.

### B3 — P1: removing a track does not invalidate its outstanding probe

Location: [OutputSession.cpp:711](../../src/worker/session/OutputSession.cpp#L711),
especially the inline removal at line 718 and later arm at line 741.
Owner: **6.23c**. Evidence: live production-worker probe.

Nonempty standalone selections are validated on the prepare executor and armed
after validation. Removal bypasses that executor and arms an empty path inline.
FIFO ordering among queued probes cannot order them against that inline path.
A selection requested before a removal can therefore finish its slow probe
later and restart audio the operator already removed.

The probe started a track, sent a second selection with a 600 ms validation delay,
then sent removal before the probe completed. Removal was acknowledged and status
reported `live=false`. After the older probe and stage finished, status became
`live=true` for the removed selection, without another operator request.

Stamp selection intent when the RPC request is accepted, including removal.
Revalidate that generation before arming or publishing asynchronous work; a
superseded probe may complete its reply but must not reinstate playback. Check
cancellation again after blocking validation as well. Cover remove-during-probe,
multiple queued probes followed by removal, and teardown during validation. The
current socket fixture removes only after the deferred validation has answered.

### B4 — P1: asynchronous seek completion invalidates already-paced video deadlines

Locations: [BackgroundTransport.cpp:1252](../../src/worker/session/BackgroundTransport.cpp#L1252),
[BackgroundTransport.cpp:1289](../../src/worker/session/BackgroundTransport.cpp#L1289),
[AvPlayer.cpp:434](../../src/worker/playback/AvPlayer.cpp#L434),
[PlaybackPipeline.cpp:308](../../src/worker/playback/PlaybackPipeline.cpp#L308).
Owner: **6.23d**. Evidence: live production-worker probe with audible transport.

`beginSeekTo()` lets the video decoder and pacer process the new target immediately.
The pacer observes the decoder's seek generation and computes its new clock offset
using the clock that still reports the pre-seek position. Hundreds of milliseconds
later, the seek flight finishes and `finishSeekTo()` rebases that clock. Nothing
invalidates the offset or submission deadlines already computed in the old clock
domain. On a backward jump, video then waits for the rebased clock to catch up to
an obsolete deadline; a forward jump can produce the opposite pacing error.

The production-worker probe played the six-second audio/video fixture to about
**1.58 s**, then sought backward to **0.20 s** with a **300 ms** audio-ack delay.
At 0.30 s after the request, the presented-frame count reached **26**. It remained
26 through the final 1.51 s sample, and reported video position remained **0.50 s**.
Throughout that stall, `media.playing=true` and `media.error` was empty. RPC
continued answering: this is a playback clock/pacing defect, not a blocked loop.

Pair video deadline anchoring with the audio clock's completed seek epoch. Hold
or discard interim submissions until the shared epoch lands, or explicitly
invalidate/re-anchor the pacer when the clock rebases. Keep request generation,
applied media target and clock-domain generation coherent. A regression must
continue playing after delayed forward/backward seeks and inspect frame progress
and media position. The new fixture immediately pauses behind its first seek,
which hides this continuing-playback failure.

### B5 — P2: successfully superseded audio stages emit a false failure

Location: [BackgroundTransport.cpp:732](../../src/worker/session/BackgroundTransport.cpp#L732).
Owner: **6.23e**. Evidence: live production-worker probe and source tracing.

When a successful stage is superseded, its player moves into `abandoned`, then
into `retireStandaloneInstance(std::move(abandoned))`. That move clears
`abandoned`, and the stage's player was already cleared by the first move.
The following `if (abandoned == nullptr && !stage->player)` therefore interprets
normal supersession as a failed bring-up and calls `standaloneFailedLocked()`.

The probe delayed A's start by 700 ms, selected B while A staged, and waited for
B to become live. B did start, but the worker also sent a `media.error` naming A
with **"standalone track: the audio transport refused it"**. Neither file failed;
A was deliberately superseded. The false notification can outlive the transient
mirror update and reach the frontend's fault display.

Preserve an explicit success/failed/superseded outcome across ownership moves,
and do not fall through from the superseded branch to error handling. Verify
that superseding a successful held player emits no failure, while a genuinely
failed current stage still reports its own reason and path.

## Previous findings and architecture assessment

| 6.22 area | Assessment |
|---|---|
| A1 shaping ownership | Per-thread FontCollections replace the shared mutable collection; retained layout handoff and one-painter ownership are documented. No new defect identified in this fix. |
| A2 GPU reclamation | The three whole-device teardown sites take the shared queue guard. Fence-based retirement and device-loss/timeouts remain explicitly M7.1b. |
| A3 outgoing audio | The handoff pauses the outgoing player before retirement. Existing deactivation evidence remains valid. |
| A4 capacity overflow | Component-level frame blocking is removed, but the deferred result is not integrated with session publication (B2). |
| A5 blocking media | File validation, metadata reads, standalone joins and seek waits have moved; new ordering, ownership and pacing interactions remain (B1/B3/B4/B5). |
| A6 standalone status | The pending-selection mirror is updated when a player is removed; successful supersession is still misclassified as failure (B5). |
| A7 thumbnails | Completion carries destination generation and extent; installation checks both and retries incompatible results. No new issue identified in that fix. |

The existing process split and document/scene boundaries remain appropriate.
The important remaining design work is a single committed output transaction
and consistent asynchronous intent/completion identities. Moving waits off-thread
is necessary, but it cannot replace those contracts. Keep retirement slots
bounded and rendering responsive while making the capacity decision atomic.
Rate-limit deferred-transition logging as routine cleanup: the current loop retries
and logs on every pass while capacity stays occupied, although the state has not
changed. This is not counted as an additional primary finding.

## Verification and limits

Both build presets completed successfully without recompilation. The existing
headless suite passed **32/32 in 54.08 s** inside the sandbox, where live audio
subcases reported skips. The desktop suite passed **55/55 in 296.92 s** with
local-session access, including all **14 display-labelled checks** (168.06
sec*proc). Presets ran sequentially. Their green results do not cover the
failures reproduced by the focused probes; [PROGRESS.md](../../PROGRESS.md)
records the current handoff.

Four temporary Python probes used the existing production binary, framed Unix
RPC and generated fixture assets already in the build tree. Requests and replies
were correlated by ID, including deferred replies. Probe workers used a windowed
output, the **AMD Radeon 890M / RADV** device and the local audio session; all
were shut down after their checks. The probes did not modify source or existing
test assertions. Faults used existing shipped test seams:

| Probe | Fault seam | Observed result |
|---|---|---|
| Deferred scene | `OPENSHOW_FAULT_PACING_EXIT_MS=3000`, match `loop-a.mp4` | C credited active/pending=0 while media still B, at 20.6 ms and 200 ms later |
| Remove during probe | `OPENSHOW_FAULT_STANDALONE_PROBE_MS=600`, match `stop-a.m4a` | Removed track becomes live again after older validation/staging completes |
| Backward seek | `OPENSHOW_FAULT_SEEK_ACK_MS=300`, match `seek-ack.mp4` | Frame count 26 and position 0.50 remain unchanged after clock completion despite playing=true |
| Successful supersession | `OPENSHOW_FAULT_AUDIO_START_MS=700`, match `stop-a.m4a` | B becomes live; false refusal notification emitted for superseded A |

The thread-ownership failure B1 is reasoned from a concrete interleaving, not a
worker crash induced during this review. B2 measured status/media disagreement;
its pixel consequence was source-traced rather than captured separately. B4
measured presented frames and position, not acoustic A/V offset. No projector,
unplug/replug, audible quality acceptance, long-run drift or service soak was
performed. Those remain M7 gates; the five findings above concern current
production behaviour and cannot be closed by reclassifying them as hardware work.
