# OpenShowQt progress and handoff

Updated: 2026-10-30 (Europe/London). Reviewed code: `0e64f0b` (6.20e) plus the
6.21 follow-ups, now complete.

## Current checkpoint

M0–M5 and the recorded M6 closure through 6.20e remain historical DONE
checkpoints. The [conformance review](docs/reviews/M6-conformance-2026-10-30.md)
tracked seven remaining gaps as **6.21a–g** in [PLAN.md](PLAN.md) — **all seven
are DONE**, each closed with its own commit, fixture and architecture row. Full
M6 conformance is now what the evidence shows; the review's surviving named
limit is keyboard automation, already a recorded skip in
[architecture.md](docs/architecture.md).

In short (each finding's detail is in the review, the PLAN rows and the
architecture table):

- **6.21a** (F1) — layout-cache slots hold `shared_ptr<const SceneLayout>`;
  a draw retains what it paints. R16 closed.
- **6.21b** (F2) — `adoptAssetPaths()` rewrites every path-bearing scene item
  across mirror, in-flight prepares, desired content and restore snapshot.
  R7 closed; real-worker coverage landed with 6.21g.
- **6.21c1** (F3 1/2) — prepared audio is staged held on the prepare executor
  (`primeAudio`) and adopted at the land: 6.4 ms present→air against a 700 ms
  scripted bring-up.
- **6.21c2** (F3 2/2) — old transports retire on a one-deep lane (join + audio
  stop off the frame tick; GPU close strictly after the join), with the
  producer-generation filter keeping the successor safe; the standalone swap
  stages its bring-up the same way. 4.5 ms present→air against a 700 ms exit,
  1.6 ms swap ack against a 700 ms staged start, audience gap ≤ 26 ms.
- **6.21d** (F5) — the RPC pump never reads a thumbnail file (one-deep decode
  lane, completion publishes on the next tick, destructor joins): status and
  release ≤ 0.6 ms through a 600 ms cold decode, shutdown mid-read drained.
  R10 fully closed.
- **6.21e** (F4) — `dropBackground()` erases `background.gradient` and
  `sceneContent()` counts it, pinned at the wire, at the acked indicator
  (peer ledger `[16744448 | flat | 16744448]`) and in audience pixels
  (#F87C00 at the floor through a black-top ramp, #000000 after the clear).
  R17's gradient qualifier closed.
- **6.21f** (F6) — `PresentationModel` reads `scene.foregroundErrors`/
  `foregroundFailureEvents`; `OutputPanel.qml` shows "picture not drawn:
  ‹path› — ‹reason›". S17 proves the projection, the repair transition and the
  transport lane's independence. R18's frontend qualifier closed.
- **6.21g** (F7) — `frontend_journey_tests` (new display fixture): the real
  `WorkerController` supervising a real worker, asserting in status AND in
  PreviewConsumer's own frame — go paints plate+picture+words, the gradient
  samples ramp-to-floor, per-layer clears compose to uniform black, the
  restore re-prepares across the 16-deep store pruning (one revision past the
  18th filler), both renamed plates are adopted without moving the audience
  and drawn again from the new paths, and a real missing-picture fault reaches
  `foregroundErrors` with its reason and leaves it on the repair.
  `production_journey_tests` stays as the raw-protocol coverage.

## Verification

| Check | Result |
|---|---|
| `cmake --build --preset headless -j 2` | PASS |
| `cmake --build --preset desktop -j 2` | PASS |
| `ctest --preset desktop` with local-session access | 55/55 PASS, 281.23 s; all 14 display-labelled checks, 163.47 sec*proc |
| `ctest --preset headless` | 32/32 PASS, 51.77 s; sandboxed, live PipeWire subscenarios internally skipped |
| `frontend_journey_tests` standalone | PASS 2×; adoption + prune + real fault/repair all green, fault reason "cannot open … for reading" projected and repaired |
| `worker_controller_tests` standalone | PASS; S16 ledger `[16744448 \| flat \| 16744448]`, S17 projection + repair with the media lane unchanged |
| `production_journey_tests` standalone | PASS; 6.21e gradient step sampled #070400/#F87C00 before the clear and #000000/#000000 after |
| `preview_selection_tests` standalone | PASS in both trees; 6.21d RPC ≤ 0.6 ms through a 600 ms decode, shutdown mid-read clean |
| `background_media_tests` standalone | PASS; 6.21c2 present→air 4.5 ms (exit 700 ms), swap ack 1.6 ms, worst audience gap 26 ms, RPC ≤ 0.6 ms |

The successful desktop run used RADV on the AMD Radeon 890M. Do not run both
ctest presets concurrently: they collide in the image-cache fixture's
PID-named temporary directory, and desktop needs local-session access.

## Next action

M6 is conformant end to end; M7 is open. **7.1a–c** restart/recovery and the
7.2 resource-budget rows remain the next planned leaves, then 7.3 packaging
and 7.4 hardware rehearsal (which owes the audible-playback, target-GPU and
hotplug claims named in the recorded skips).
