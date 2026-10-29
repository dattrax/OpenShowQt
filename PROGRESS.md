# OpenShowQt progress and handoff

Updated: 2026-10-29 (Europe/London).

## Current checkpoint

**M0–M5 are historical implementation checkpoints. M6 is incomplete.**
The committed checkpoint is task **6.18a1** at `1b54bdb`: SceneStore owns
prepared scenes and desired/landed/published output state. Most M6 fixes through
that task are DONE, but **6.9a/b remain TODO**. The authoritative task/status
ledger is [PLAN.md](PLAN.md#m6--code-review-remediation-and-architecture).
Productization, recovery, packaging and full hardware acceptance remain M7.

**Next implementation task: 6.18a2**, the previously recorded PreviewProducer
extraction. The checkout already contains untracked preparation for that task;
it is not wired into worker CMake or OutputSession and has not been accepted.
This documentation/architecture audit does not mark 6.18a2 or 6.20 complete.

## Completed capabilities

| Checkpoint | Delivered | Remaining qualification |
|---|---|---|
| M0 | Plan, FreeShow reference, build/dependency foundation | Current dependency recipe is in CMake and `scripts/build-skia.sh`; packaging notices remain M7. |
| M1 | Separate worker, output/display selection, RPC, shared-memory previews, decode/upload/audio feasibility | Historical benchmarks and display evidence are archived; full projector/Wayland/X11 acceptance remains open. |
| M2 | Documents, service packages, history/recovery, compiled scenes | Later M6 fixes govern current durability, ownership and validation behavior. |
| M3 | Lyric authoring, typography, backgrounds, transitions, clear/restore | Later acknowledgement, fade and restore fixes supersede the original implementation notes. |
| M4 | Workspace, libraries, editor/style controls, shortcuts/settings | Real selected-scene previews and multi-item authoring were completed later in M6. |
| M5 | PlaybackPipeline, AvPlayer, video continuity, source controls and media errors | Production controls/standalone playback were completed later in M6; cold-go scheduling is still open. |
| M6 completed work | Production compositor, public Skia/ICU/build recipe, correlated RPC, honest presented state, preview/GPU ownership, audio/seek fixes, bounded transport/cache, durable assets/autosave, production media controls, worker editor/thumbnails, multi-item/fit/gradient authoring and SceneStore | See the individual DONE rows in PLAN.md; later completion does not waive 6.9 or M7 gates. |

## Architecture audit and optimization, 2026-10-29

[docs/architecture.md](docs/architecture.md) records current owners, source/test
entry points, protected contracts and differences from the intended design.
The original [M6 review](docs/M6-code-review.md) remains historical evidence;
it is not a list of findings that are all still unfixed.

The two-process split, Qt-free worker/domain, immutable scene boundary, bounded
metadata RPC, shared-memory previews, worker text layout and single-target
audience composition match the intended design. Important open differences:

- **6.9a:** prepare-time decode/probe still blocks the RPC dispatch thread.
- **6.9b:** transport start/pre-roll/retirement, cold text layout and first blur
  generation still cost render-thread time. Preview readback is synchronous;
  deferring later attempts bounds average work, not one-frame latency.
- **6.20:** production images and text share the words layer's ordered stack;
  the images toggle does not control authored foreground images. Independent
  layer controls need reconciliation with mixed item ordering.
- **6.20:** standalone and video use independent AvPlayers/PipeWire streams,
  differing from the plan's one software-mixed output stream. Explicitly accept
  that design or implement the shared sink before closing the review.
- **6.19b/6.20:** background autosave is asynchronous; explicit save/load and
  decision/quit drains remain synchronous. Missing foreground-image failures
  are logged but lack acknowledged per-item UI reporting.
- **6.18a2/b, 6.19a/b:** the remaining ownership extractions are unfinished.
  **7.1–7.4:** recovery, resource/latency measurements, packaging and service/
  physical-display acceptance remain open. Pinned cache excess is reported,
  not a hard bound on total process memory.

The completed SceneStore path was optimized: it now copies each Scene once at
prepare, outside its lock, then shares that immutable instance with pins and
frame snapshots. This removes repeated deep copies and the separate capture
cache; the same revision can reuse the renderer's pointer-keyed layout cache
across audience and selected-preview drawing. Tests verify identity, isolation
from later caller edits, eviction survival and existing publication/thread rules.
No throughput or percentage speedup is claimed. The History lifetime comment was
also corrected: Documents must outlive the History that observes it.

PLAN.md now keeps concise deliverables/acceptance pointers for every completed
task. **All 122 task IDs and statuses are preserved**, as are unresolved task
acceptance and release gates. The previous full plan/progress files are preserved
under [docs/history](docs/history/README.md), including old commands, measurements,
decisions, mutation narratives, display caveats and superseded handoffs.

## Verification for this audit

- `CCACHE_DISABLE=1 cmake --build --preset headless -j 2`: PASS.
- `CCACHE_DISABLE=1 cmake --build --preset desktop -j 2`: PASS.
- `ctest --preset headless --output-on-failure`: **28/28 PASS**, 48.29 s.
  Vulkan used llvmpipe; live PipeWire subscenarios reported daemon-access skips.
- `ctest --preset desktop --output-on-failure`: **49/49 PASS**, 231.42 s,
  including all 12 display-labelled tests, Radeon 890M/RADV Vulkan and live
  PipeWire subscenarios. The first sandboxed run was interrupted after socket
  fixtures reported `Operation not permitted`; this full rerun used local access.
- Documentation ledger comparison: all task IDs/statuses unchanged; original
  snapshots preserved. `git diff --check`: PASS.

These checks do not establish projector placement/DPI/stacking, audible playback,
target-GPU timing, a thirty-minute A/V run or a one-hour mixed service soak.
Historical desktop/headless totals in archived logs are not fresh results.

## Working tree and exact next action

This audit changes PLAN.md/PROGRESS.md, adds the architecture audit/history
snapshots, labels the original M6 review as historical, optimizes SceneStore
with focused regression coverage, and updates related ownership/cache comments.
The user subsequently authorized committing and publishing these changes,
including the rule keeping PLAN.md concise. No milestone tags are being added.

Pre-existing untracked work was left untouched:

- `src/worker/session/{Faults.h,PreviewProducer.h,PreviewProducer.cpp}`
- `tests/preview_producer_tests.cpp`
- `audio-mix-fixtures/`, `av-sync-fixtures/`, `direct-fixtures/`

Resume by inspecting Git status/history, reading the **6.18a2** acceptance and
reviewing that untracked extraction against the existing OutputSession preview
owner. Wire and test one owner for preview lifecycle/selection/thumbnail/budget
policy; preserve frame revision and slot ownership. Keep 6.9a/b visible and
unfinished until their actual scheduling checks pass. Do not claim M6 closure
until 6.20 resolves the recorded architecture differences and remaining gates.
