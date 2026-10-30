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

## Remaining 6.21 gaps

F3 (audio startup/pre-roll and transport retirement on the render thread),
F5 (cold thumbnail decode on the RPC thread), F4 (gradient clears),
F6 (foreground-error visibility) and F7 (real frontend journey fixture).
The review holds source locations, reproduced evidence and required
regressions for each.

## Verification

| Check | Result |
|---|---|
| `cmake --build --preset headless -j 2` | PASS |
| `cmake --build --preset desktop -j 2` | PASS |
| `ctest --preset desktop` with local-session access | 54/54 PASS, 251.55 s; all 13 display-labelled checks, 137.26 sec*proc |
| `ctest --preset headless` | 32/32 PASS, 51.65 s; sandboxed, live PipeWire subscenarios internally skipped |
| `scene_render_tests` standalone | PASS 3× in each build tree (churn fixture included) |

The successful desktop run used RADV on the AMD Radeon 890M. Do not run both
ctest presets concurrently: they collide in the image-cache fixture's
PID-named temporary directory, and desktop needs local-session access.

## Next action

**6.21c** (F3) is split before starting (plan rule 2 — the retirement half
needs a handoff protocol that preserves R6's quiesce boundary, which puts
the whole leaf past a day):

- **6.21c1** — staged audio readiness: the prepare executor builds a
  pre-started (held) `AvPlayer` for the primed path — probe answer, feeders,
  sink negotiation, pre-roll — and `applyBackground()` adopts it at the land
  instead of probing and starting on the render thread. GPU attach stays on
  the render thread. Fixture: scripted slow audio start with audience frame
  gaps observed across a primed present.
- **6.21c2** — retirement: old pipeline stop/join and audio close move off
  the frame tick behind an explicit handoff; `close()` (GPU destruction)
  stays on the render thread; the standalone swap at
  `BackgroundTransport.cpp:580–606` stops doing synchronous stop/start under
  the transport mutex. Slow-retirement and status-responsiveness fixtures.

Then 6.21d–g. M7.1a–c recovery, 7.2 budgets, 7.3 packaging and 7.4 hardware
rehearsal remain TODO.
