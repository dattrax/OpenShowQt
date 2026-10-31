# OpenShowQt progress and handoff

Updated: 2026-10-31 (Europe/London). Reviewed code: `db406f7` (6.22g).

## Current checkpoint

The [post-6.22 review](docs/reviews/M6-post-6.22-review-2026-10-31.md) introduced
**6.23a–e**. **6.23a is DONE**; b–e remain open. The retirement claim reserves
capacity before consuming a superseded stage; its controlled arrival regression
passed with real audio. Current evidence: [6.23 verification](docs/reviews/M6-6.23-verification-2026-10-31.md).

Manual output fix: disabled GLFW's fullscreen auto-iconify on focus loss, so
activating the operator UI does not ask Wayland to minimize the audience window.
Both builds and three targeted desktop checks passed; the app was relaunched.
The user confirmed that switching focus now keeps the output visible.

Main-screen follow-up: charcoal/blue ProPresenter-inspired workspace, library and
running order left, slide grid centre, persistent audience monitor right.
Slide/text authoring and destructive document shortcuts are now Edit-only;
hover resizing, displaced drag rows and library focus routing are corrected.
The supplied ProPresenter [UI guide](https://support.renewedvision.com/hc/en-us/articles/360041345954-Understanding-The-ProPresenter-User-Interface)
now supplements the screenshot; remaining layout differences are recorded in
[the reference](docs/reference.md), especially the sidebar split and bottom media bin.

## Review summary

Four P1s concern standalone retirement ownership, premature scene credit during
media deferral, track removal undone by an older probe, and video deadlines left
in the old clock domain after an asynchronous seek. One P2 reports successful
supersession as an audio failure. Four defects were reproduced against the real
worker; the ownership race is source-confirmed without inducing its crash.
The direct shaping, queue-guard, audio-deactivation and thumbnail-destination
fixes from 6.22 are present; their evidence is retained.

## Verification

| Check | Result |
|---|---|
| Review baseline: both build presets, `-j 2` | PASS; no recompilation needed at review time |
| `ctest --preset headless` | 32/32 PASS, 54.08 s; sandboxed, live audio subcases internally skip |
| `ctest --preset desktop` with local-session access | 55/55 PASS, 296.92 s; all 14 display-labelled checks, 168.06 sec*proc |
| Main-screen follow-up | Both builds PASS; five focused checks PASS (12.44 s), including workspace interaction regressions; production-worker thumbnails verified and captures inspected at 1000×680, 1380×800 and 1680×1020 |
| Four focused production-worker probes | Reproduced B2–B5; request/reply IDs correlated, existing fault seams and generated fixtures used |
| Documentation checks | Link targets, retained task statuses, archive integrity and whitespace checked before handoff |

Probes used RADV on the AMD Radeon 890M and the local audio session. B2 measured
scene/media status disagreement; pixel mismatch was source-traced. B4 measured
frame-count/position stalls, not acoustic drift. The potential B1 thread-destructor
crash was not induced. No projector, hotplug, long drift or service soak was run.
Presets ran sequentially to avoid shared temporary-fixture collisions.

## Evidence and next action

The full 6.22 implementation/verification narrative is preserved in
[history](docs/history/PROGRESS-through-6.22g.md). PLAN.md holds concise task rows;
source analysis, probe observations and required regressions are in the review.
The review itself left production code and tests unchanged; subsequent manual
fixes update OutputWindow and the QML workspace, with interaction regressions.

Start **6.23a–d** with atomic retirement reservation, session-level scene commit,
intent generations and coordinated seek-clock epochs; then 6.23e's outcome fix.
Add focused regressions with the changes, then rerun existing suites. M7 recovery,
budgets, packaging and physical/long-service acceptance remain TODO.
