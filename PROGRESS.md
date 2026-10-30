# OpenShowQt progress and handoff

Updated: 2026-10-30 (Europe/London). Reviewed code: `8402e69` (6.21g).

## Current checkpoint

The [architecture re-review](docs/reviews/M6-architecture-follow-up-2026-10-30.md)
found seven remaining issues. Preserve the recorded 6.21 DONE checkpoints; complete
**6.22a–g** in [PLAN.md](PLAN.md) before claiming full conformance or advancing to M7.

## Review summary

Four P1 issues: shared mutable font resolution across shaping threads; GPU queue
synchronization during retirement; old audio continuing through retirement; and
render-thread waiting when a second retirement overlaps. Three P2 issues concern
remaining synchronous media operations, stale standalone status and thumbnail
completion compatibility. Source locations and proposed contracts are in the review.

Foreground path adoption, gradient clears, UI foreground fault reporting and the
real-controller journey address their earlier findings. Retained layout handles
fix F1's lifetime defect, but not A1's separate shaping-cache race.

## Verification and retained evidence

This pass inspected source, commit diffs, pinned Skia internals, the installed
Khronos Vulkan registry and fixture coverage. **No builds, runtime probes or test
suites were run**, keeping the requested architecture review ahead of testing.
Historical 55/55 desktop and 32/32 headless results and 6.21 completion details
are preserved in [history](docs/history/PROGRESS-through-6.21g.md).
Documentation links, retained task statuses and `git diff --check` were verified.

Only documentation changed in this pass. PLAN.md keeps brief task summaries;
implementation evidence belongs in the review, architecture or history files.

## Next action

Start **6.22a–d** with safe shaping ownership and transport handoff/reclamation;
then close 6.22e–g. Add focused regressions with the fixes before broader suites
and physical acceptance. Split 6.22e into small leaves before starting it.
M7 recovery, budgets, packaging and hardware/long-service acceptance remain TODO.
