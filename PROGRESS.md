# OpenShowQt progress and handoff

Updated: 2026-10-31 (Europe/London).

## Current checkpoint

Current UI work: **UI.3a DONE**, verified 2026-10-31 (cue groups per entry with
stable repeated-entry ids, arbitrary-show compilation that never moves the
editing cursor; gates 57/57 and 32/32). The
[main-display plan](docs/ui/operator-workspace-plan.md)
audits every current control and defines the remaining leaves; the next leaf is
**UI.3b** (Show triggers, running-order navigation and live markers).

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
| Full headless suite with local GPU/PipeWire access | 32/32 PASS, 66.02 s; live-audio cases executed |
| Full desktop suite with local-session access | 57/57 PASS, 339.21 s; all 15 display-labelled checks |
| New focused regressions | Retirement claim race; deferred replacement/clear/cancellation with frontend preview pixels; removal during queued probes and shutdown; continued/paused seek playback; stage outcomes and transient notifications |
| UI.1 acceptance (2026-10-31) | `workspace_ui_tests` visible-tree audit (Show: no setup controls, only `librarySearchField` writable; Settings exposes routing), picker-URL regressions in `worker_controller_tests`/`document_controller_tests`, `ui_loads` + guards; evidence in [the plan](docs/ui/operator-workspace-plan.md) |
| UI.2 acceptance (2026-10-31) | `workspace_ui_tests` outline/detail interactions (duplicate entry ids, 180-char names, scrolling identities, drag onto a playlist row) and `document_present_tests` browsing-never-goes-live on the wire; evidence in [the plan](docs/ui/operator-workspace-plan.md) |
| UI.3a acceptance (2026-10-31) | `document_controller_tests` cue-group projection (duplicate shows, move/remove identity stability) and arbitrary-show compilation leaving the editing cursor fixed; evidence in [the plan](docs/ui/operator-workspace-plan.md) |
| Documentation | Current links, historical archive integrity and whitespace checked |

Presets ran sequentially. Frame/position progress was measured; acoustic offset,
long drift, projector hotplug and full-service soak were not measured here.
Those remain M7 acceptance work.

## Operator workspace

The fullscreen focus/minimize fix was confirmed by the user. The main workspace
uses the supplied ProPresenter palette/layout direction; Show is for presenting,
with slide authoring and destructive document shortcuts confined to Edit. The
sidebar outline/detail split is in place (UI.2); the dedicated bottom media bin
and right-hand control arrangement remain UI follow-ups in
[the reference](docs/reference.md).

## Evidence and next work

Keep PLAN.md updates to concise task summaries. Implementation and verification
details belong in the review documents; older detailed records remain in
[history](docs/history/README.md), including the unchanged [6.22g handoff](docs/history/PROGRESS-through-6.22g.md).
Next is UI.3b (Show triggers, running-order navigation and live markers), then
UI.3c and UI.4–6. M7 remains the release milestone.
