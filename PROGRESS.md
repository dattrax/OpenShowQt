# OpenShowQt progress and handoff

Updated: 2026-10-30 (Europe/London). Reviewed code: `0e64f0b` (6.20e) plus the
6.21 follow-ups landing on top.

## Current checkpoint

M0–M5 and the recorded M6 closure through 6.20e remain historical DONE
checkpoints. The [conformance review](docs/reviews/M6-conformance-2026-10-30.md)
tracked seven remaining gaps as **6.21a–g** in [PLAN.md](PLAN.md); complete
them all before claiming full M6 conformance or moving to M7.

**6.21a is DONE**: the layout cache now hands out retained handles
(`shared_ptr<const SceneLayout>`), replacing or evicting a slot releases only
the cache's own reference, and `SceneItemsLayer` holds what it draws across
the whole frame. The regression drives eviction, same-key replacement and a
concurrent prepare-vs-draw churn in `scene_render_tests` (exact pixel
equality per frame). F1's R16 row is closed in
[architecture.md](docs/architecture.md).

**6.21b is DONE**: `adoptAssetPaths()` rewrites every path-bearing scene
item, not just `background.assetPath`, across the revision mirror,
in-flight prepares, desired content and the restore snapshot. The scripted
peer now keeps a per-prepare item-path ledger, and the new
`worker_controller_tests` scenario drives present→adopt→clear-all→restore
twice — once adopting while the go's prepare is still in flight — asserting
the deleted path is never named again and both restores carry the service
path. F2's R7 row is closed; the real-worker adoption run belongs to 6.21g.

**6.21c1 is DONE** (F3 half 1): a prepared clip's audio half is staged on
the prepare executor beside the file prime — `primeAudio` starts a held
`AvPlayer` (`Config::startHeld`: connection, feeders, mixer, pre-roll all
paid off-loop) and `applyBackground` adopts and releases it at the land
(measured 6.4 ms present→air against a 700 ms scripted bring-up; audience
frames kept a 50 ms worst gap; the adopted lane read live). Audio instances
are now swap-owned: `retireAudioInstance()` swaps under `fMutex` and stops
outside it. `background_media_tests` owns the regression. What remains of
F3 is **6.21c2**: retiring the OLD transport (stop/join/close) and the
standalone swap's synchronous stop/start under the transport mutex.

## Remaining 6.21 gaps

F3 half 2 (6.21c2, retirement), F5 (cold thumbnail decode on the RPC
thread), F4 (gradient clears), F6 (foreground-error visibility) and F7
(real frontend journey fixture). The review holds source locations,
reproduced evidence and required regressions for each.

## Verification

| Check | Result |
|---|---|
| `cmake --build --preset headless -j 2` | PASS |
| `cmake --build --preset desktop -j 2` | PASS |
| `ctest --preset desktop` with local-session access | 54/54 PASS, 252.35 s; all 13 display-labelled checks, 138.39 sec*proc |
| `ctest --preset headless` | 32/32 PASS, 51.77 s; sandboxed, live PipeWire subscenarios internally skipped |
| `scene_render_tests` standalone | PASS 3× in each build tree (churn fixture included) |
| `background_media_tests` standalone | PASS; new 6.21c1 scenario measured ack 715 ms off-loop, worst audience gap 50 ms, present→air 6.4 ms, lane live |

The successful desktop run used RADV on the AMD Radeon 890M. Do not run both
ctest presets concurrently: they collide in the image-cache fixture's
PID-named temporary directory, and desktop needs local-session access.

## Next action

**6.21c2** (F3 half 2): retire the OLD transport off the frame tick. Today
`teardownBackgroundTransport()` still runs `requestStop()/join()`, the seam
reset, `close()` and `retireAudioInstance()` synchronously on the render
thread inside `applyBackground()`, and the standalone swap
(`BackgroundTransport.cpp` ~580–606) stop/starts synchronously while holding
the transport mutex, delaying RPC status readers. Design the handoff so
R6's explicit quiescence survives (the render thread must still own the
seam when the successor starts, and GPU attach/destruction stays on the
render thread), then add fixtures for slow retirement and status
responsiveness. Then 6.21d–g. M7.1a–c recovery, 7.2 budgets, 7.3 packaging
and 7.4 hardware rehearsal remain TODO.
