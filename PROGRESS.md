# OpenShowQt progress and handoff

Updated: 2026-10-30 (Europe/London). Reviewed code: `0e64f0b` (6.20e).

## Current checkpoint

M0–M5 and the recorded M6 closure through 6.20e remain historical DONE
checkpoints. The fresh [conformance review](docs/reviews/M6-conformance-2026-10-30.md)
found seven remaining gaps, now tracked as **6.21a–g** in [PLAN.md](PLAN.md).
Complete those follow-ups before claiming full M6 conformance or moving to M7.

## Review summary

Three P1 issues: concurrent layout replacement can invalidate a draw; foreground
image paths escape saved-scene adoption; audio startup/pre-roll and transport
retirement still block rendering. Four P2 gaps concern gradient clears, thumbnail
RPC blocking, foreground-error visibility and the real-frontend journey fixture.
The review contains source locations, reproduced evidence and required regressions.

## Verification

| Check | Result |
|---|---|
| `cmake --build --preset headless -j 2` | PASS; no work to do |
| `cmake --build --preset desktop -j 2` | PASS; no work to do |
| `ctest --preset headless` | 32/32 PASS, 47.92 s; sandboxed, live PipeWire subscenarios internally skipped |
| `ctest --preset desktop` with local-session access | 54/54 PASS, 248.13 s; all 13 display-labelled checks, 137.54 sec*proc |
| Standalone production-library probes | Reproduced layout-reference replacement, stale foreground adoption/restore paths and retained gradient after background clear |
| Documentation checks | Existing task IDs/statuses preserved; archive snapshots exact; local links and `git diff --check` verified |

The successful desktop run used RADV on the AMD Radeon 890M; its live audio
restart/sink scenarios ran. This does not establish audible two-stream pairing,
projector placement/hotplug, Wayland/X11 physical acceptance or the long M7 soaks.
The journey remains silent and bypasses WorkerController (review F7).

The initial sandboxed desktop attempt failed 15 checks: local socket/session
restrictions affected controller/display fixtures, and concurrent headless/desktop
runs collided in the image-cache fixture's PID-named temporary directory. The
subsequent desktop run, after headless finished and with local-session access,
passed all 54 unchanged tests. Do not run both presets concurrently in sandboxes
that share temporary storage and reuse process IDs.

## Documentation and retained evidence

PLAN.md now holds brief deliverable/acceptance rows, corrected layer/audio status
and the 6.21 follow-ups. Its rule 8 limits future task updates to 2–4 lines;
implementation narratives and detailed measurements belong outside the plan.
This handoff replaces the long completed log. Exact pre-review snapshots of both
files are in [history](docs/history/README.md); architecture documentation links
the new findings and qualifies the older closure claims. Production code and
existing tests were not changed.

## Next action

Inspect Git status and the review, then start **6.21a**: retain layouts across
cache replacement and add a concurrent prepare/draw regression. Follow with
6.21b/c and the remaining 6.21 tasks; split large scheduling work before starting.
M7.1a–c recovery, 7.2 budgets, 7.3 packaging and 7.4 hardware rehearsal remain TODO.

Suggested documentation commit: `docs: review M6 conformance and condense task handoff`.
