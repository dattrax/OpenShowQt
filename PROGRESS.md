# OpenShowQt progress and handoff

Updated: 2026-10-31 (Europe/London).

## Current checkpoint

Current UI work: **UI.1 IN PROGRESS**. The [main-display plan](docs/ui/operator-workspace-plan.md)
now audits every current control and defines UI.2–6 before further layout work.
Audience setup is moving to Settings; Show retains runtime cues and concise status.


**M6 async integration follow-ups 6.23a–e are DONE**, with one fix commit per item.
[Verification and regressions](docs/reviews/M6-6.23-verification-2026-10-31.md)
close the five historical findings in the [post-6.22 review](docs/reviews/M6-post-6.22-review-2026-10-31.md).
M7 recovery, budgets, packaging and service acceptance remain TODO in [PLAN.md](PLAN.md).

Retirement ownership is reserved before a stage is consumed; deferred scenes
retain matching media, revision and pixels until their transaction lands.
Standalone generations invalidate removed requests; video deadlines use the
completed seek-clock epoch; supersession and failure have distinct outcomes.

## Verification

| Check | Result |
|---|---|
| Desktop and headless build presets, `-j 2` | PASS |
| Full headless suite with local GPU/PipeWire access | 32/32 PASS, 65.98 s; live-audio cases executed |
| Full desktop suite with local-session access | 57/57 PASS, 335.62 s; all 15 display-labelled checks |
| New focused regressions | Retirement claim race; deferred replacement/clear/cancellation with frontend preview pixels; removal during queued probes and shutdown; continued/paused seek playback; stage outcomes and transient notifications |
| Documentation | Current links, historical archive integrity and whitespace checked |

Presets ran sequentially. Frame/position progress was measured; acoustic offset,
long drift, projector hotplug and full-service soak were not measured here.
Those remain M7 acceptance work.

## Operator workspace

The fullscreen focus/minimize fix was confirmed by the user. The main workspace
uses the supplied ProPresenter palette/layout direction; Show is for presenting,
with slide authoring and destructive document shortcuts confined to Edit.
The sidebar outline/detail split, dedicated bottom media bin and right-hand
control arrangement remain UI follow-ups in [the reference](docs/reference.md).

## Evidence and next work

Keep PLAN.md updates to concise task summaries. Implementation and verification
details belong in the review documents; older detailed records remain in
[history](docs/history/README.md), including the unchanged [6.22g handoff](docs/history/PROGRESS-through-6.22g.md).
Current priority is UI.1 followed by UI.2–6. M7 remains the release milestone.
