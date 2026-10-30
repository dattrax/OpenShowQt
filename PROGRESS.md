# OpenShowQt progress and handoff

Updated: 2026-10-30 (Europe/London). Reviewed code: `0e64f0b` (6.20e) plus the
6.21 follow-ups landing on top.

## Current checkpoint

M0–M5 and the recorded M6 closure through 6.20e remain historical DONE
checkpoints. The [conformance review](docs/reviews/M6-conformance-2026-10-30.md)
tracked seven remaining gaps as **6.21a–g** in [PLAN.md](PLAN.md); complete
them all before claiming full M6 conformance or moving to M7.

**6.21a is DONE**: the layout cache hands out retained handles
(`shared_ptr<const SceneLayout>`); F1's R16 row is closed in
[architecture.md](docs/architecture.md).

**6.21b is DONE**: `adoptAssetPaths()` rewrites every path-bearing scene
item; the scripted-peer fixture drives present→adopt→clear-all→restore
twice. F2's R7 row is closed; the real-worker adoption run belongs to 6.21g.

**6.21c1 is DONE** (F3 half 1): a prepared clip's audio half is staged on
the prepare executor beside the file prime (`primeAudio`,
`Config::startHeld`), and `applyBackground` adopts and releases it at the
land (6.4 ms present→air against a 700 ms scripted bring-up). Audio
instances are swap-owned: `retireAudioInstance()` swaps under `fMutex`,
stops outside it.

**6.21c2 is DONE** (F3 half 2): teardown now requests the producer's stop,
nulls the (atomic) presenter and moves pipeline plus audio player into a
one-deep retirement lane whose thread joins the pacing thread and stops the
player; `reapRetirement()` closes the GPU half on the render thread strictly
after the join. `submitVideo(submission, producer)` filters submissions by
producer generation — retired work is dropped, its slots released by the
pipeline's own close — which is what lets the successor start while the old
thread still exits; R6's ordering (close follows join) is kept, only its
wait moved. The standalone swap claims its instance under `fMutex` in
microseconds and stages the new bring-up held on a lane until a later pass
publishes it (one stage at a time; a newer intent stays pending). Shutdown
drains both lanes. Measured through the wire: present→air 4.5 ms against a
700 ms scripted pacing exit, bed-swap ack 1.6 ms against a 700 ms staged
start, audience frame gap ≤ 26 ms and status answered ≤ 0.6 ms throughout
(`background_media_tests`: `exerciseSlowRetirement`,
`exerciseStandaloneSwapOffLoop`, fault knob `OPENSHOW_FAULT_PACING_EXIT_MS
/_MATCH`).

**6.21d is DONE** (F5): the RPC pump no longer reads thumbnail files.
`pumpThumbnailDecode()` collects a finished decode (install + settle by
path, the old inline tail) or hands the next path to a one-deep lane thread
and returns — completion publishes on the next tick (the loop heartbeats at
50 ms); one file in flight keeps the memory/lookahead story unchanged, and
the destructor joins the lane. Measured through the wire against a 600 ms
scripted cold decode (`OPENSHOW_FAULT_THUMBNAIL_DECODE_MS/_MATCH`): status
and release answered ≤ 0.6 ms in 16/16 samples with `decodePending` honestly
set, the decoded backdrop pixels verified in the slot, and a shutdown issued
mid-read answered immediately with a clean drained exit
(`preview_selection_tests` phase three; `preview_producer_tests` adapted
with a pump-until-settled helper). R10 is fully closed in
[architecture.md](docs/architecture.md).

## Remaining 6.21 gaps

F4 (6.21e, gradient clears), F6 (6.21f, foreground-error visibility) and
F7 (6.21g, real frontend journey fixture). The review holds source
locations, reproduced evidence and required regressions for each.

## Verification

| Check | Result |
|---|---|
| `cmake --build --preset headless -j 2` | PASS |
| `cmake --build --preset desktop -j 2` | PASS |
| `ctest --preset desktop` with local-session access | 54/54 PASS, 258.84 s; all 13 display-labelled checks, 144.77 sec*proc |
| `ctest --preset headless` | 32/32 PASS, 51.57 s; sandboxed, live PipeWire subscenarios internally skipped |
| `preview_selection_tests` standalone | PASS in both trees; 6.21d scenario measured RPC ≤ 0.6 ms through a 600 ms decode, shutdown mid-read clean |
| `background_media_tests` standalone | PASS; 6.21c2 scenarios measured present→air 4.5 ms (exit 700 ms), swap ack 1.6 ms, worst audience gap 26 ms, RPC ≤ 0.6 ms |

The successful desktop run used RADV on the AMD Radeon 890M. Do not run both
ctest presets concurrently: they collide in the image-cache fixture's
PID-named temporary directory, and desktop needs local-session access.

## Next action

**6.21e** (F4): `dropBackground()` (`src/ui/SceneEdit.cpp`) must clear
`background.gradient` too, and `sceneContent()` must count it — with
fixtures on final pixels and acked button state. Then 6.21f–g. M7.1a–c
recovery, 7.2 budgets, 7.3 packaging and 7.4 hardware rehearsal remain TODO.
