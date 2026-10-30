# OpenShowQt progress and handoff

Updated: 2026-10-30 (Europe/London). Reviewed code: `0e64f0b` (6.20e) plus the
6.21 follow-ups landing on top.

## Current checkpoint

M0–M5 and the recorded M6 closure through 6.20e remain historical DONE
checkpoints. The [conformance review](docs/reviews/M6-conformance-2026-10-30.md)
tracked seven remaining gaps as **6.21a–g** in [PLAN.md](PLAN.md); **only 6.21g
is left** before full M6 conformance.

- **6.21a** (F1) — the layout cache hands out retained handles; R16 closed.
- **6.21b** (F2) — `adoptAssetPaths()` rewrites every path-bearing scene item;
  R7 closed (real-worker adoption coverage joins the 6.21g journey).
- **6.21c1** (F3 1/2) — a prepared clip's audio half is staged held on the
  prepare executor (`primeAudio`, `Config::startHeld`) and adopted at the land:
  6.4 ms present→air against a 700 ms scripted bring-up.
- **6.21c2** (F3 2/2) — teardown hands pipeline plus audio player to a
  one-deep retirement lane (join + audio stop off the frame tick), with
  `reapRetirement()` closing the GPU half on the render thread strictly after
  the join; the atomic presenter's producer-generation filter is what lets the
  successor start while the old thread still exits (R6's ordering kept, only its
  wait moved). The standalone swap claims its instance in microseconds and
  stages the new bring-up held on a lane. Measured through the wire:
  present→air 4.5 ms against a 700 ms scripted pacing exit, bed-swap ack 1.6 ms
  against a 700 ms staged start, audience gap ≤ 26 ms, status ≤ 0.6 ms
  (`exerciseSlowRetirement`, `exerciseStandaloneSwapOffLoop`,
  `OPENSHOW_FAULT_PACING_EXIT_MS/_MATCH`).
- **6.21d** (F5) — the RPC pump never reads a thumbnail file: it collects a
  finished decode or hands the next path to a one-deep lane thread and returns;
  the destructor joins the lane. Against a 600 ms scripted cold decode, status
  and release answered ≤ 0.6 ms in 16/16 samples with `decodePending` honestly
  set, decoded backdrop pixels verified, shutdown mid-read answered and drained
  clean (`OPENSHOW_FAULT_THUMBNAIL_DECODE_MS/_MATCH`). R10 fully closed.
- **6.21e** (F4) — `dropBackground()` erases `background.gradient` (absence is
  the flat answer the codec defines) and `sceneContent()` counts a present
  gradient. Pinned at the wire level (`scene_edit_tests`, both shapes), at the
  acked indicator with the peer's per-prepare ledger
  (`worker_controller_tests` S16: `[16744448 | flat | 16744448]`), and in the
  audience pixels (`production_journey_tests`: #F87C00 at the floor through a
  black-top ramp, #000000 at both ends after the clear). R17's gradient
  qualifier closed.
- **6.21f** (F6) — `PresentationModel` reads `scene.foregroundErrors` and
  `foregroundFailureEvents` (entry-checked, capped at the worker's 8, cleared
  with the connection), `WorkerController` re-exports them as `sceneChanged`
  properties, and `OutputPanel.qml` shows "picture not drawn: ‹path› —
  ‹reason›" with the more-count and event tally. `worker_controller_tests` S17
  (scripted peer scenario `foreground`) proves the projection verbatim, a
  second path extending list and count, the notice clearing on an acknowledged
  repair while the monotonic count stays counted, and the transport's own fault
  line unchanged throughout; `ui_loads` guards the new binding. R18's frontend
  qualifier closed.

## Remaining 6.21 gap

F7 (6.21g): the production journey drives go/clear/restore through
hand-written socket helpers instead of the real `WorkerController`/
`PresentationModel`, restores a remembered worker revision rather than
re-preparing retained content, and never exercises production asset adoption
or restore retention across store pruning. The review holds the locations and
the required coverage.

## Verification

| Check | Result |
|---|---|
| `cmake --build --preset headless -j 2` | PASS |
| `cmake --build --preset desktop -j 2` | PASS |
| `ctest --preset desktop` with local-session access | 54/54 PASS, 263.17 s; all 13 display-labelled checks, 144.91 sec*proc |
| `ctest --preset headless` | 32/32 PASS, 51.63 s; sandboxed, live PipeWire subscenarios internally skipped |
| `worker_controller_tests` standalone | PASS; 6.21e ledger `[16744448 \| flat \| 16744448]`, 6.21f projection + repair transition with the media lane unchanged |
| `production_journey_tests` standalone | PASS; 6.21e gradient step sampled #070400/#F87C00 before the clear and #000000/#000000 after |
| `preview_selection_tests` standalone | PASS in both trees; 6.21d RPC ≤ 0.6 ms through a 600 ms decode, shutdown mid-read clean |
| `background_media_tests` standalone | PASS; 6.21c2 present→air 4.5 ms (exit 700 ms), swap ack 1.6 ms, worst audience gap 26 ms, RPC ≤ 0.6 ms |

The successful desktop run used RADV on the AMD Radeon 890M. Do not run both
ctest presets concurrently: they collide in the image-cache fixture's
PID-named temporary directory, and desktop needs local-session access.

## Next action

**6.21g** (F7): a display fixture that drives the real
`WorkerController`/`PresentationModel` journey against a real worker — go,
per-layer clears, restore (re-preparing retained content, including across
store pruning) and asset adoption of background and foreground images — with
pixel and `output.status` assertions; keep `production_journey_tests` as the
raw-protocol coverage it is. Then align the PLAN header and this handoff with
the full-conformance evidence. M7.1a–c recovery, 7.2 budgets, 7.3 packaging
and 7.4 hardware rehearsal remain TODO.
