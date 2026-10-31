# OpenShowQt progress and handoff

Updated: 2026-10-31 (Europe/London).

## Current checkpoint

Current UI work: **UI.3c DONE**, verified 2026-10-31 (the Show heading's size
control steps the thumbnail cells and the ask follows the tile inside the
envelope the wire accepts — deduplicated, capped at the worker's 32 scenes and
coalesced to one ask at the scroll destination — plus a focus fix so leaving
Edit hands the presenter's keys back; gates 57/57 and 32/32). UI.3 is closed.
The [main-display plan](docs/ui/operator-workspace-plan.md)
audits every current control and defines the remaining leaves; the next leaf is
**UI.4a** (media collections, cue browsing and trustworthy type metadata).

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
| UI.3b acceptance (2026-10-31) | `workspace_ui_tests` cue-grid projection (19 rows spanning two shows), click-selects-without-air, presenter-key boundary walk and browse-one-group-off-air; `frontend_journey_tests` §7 cue lane: pending synchronously, LIVE only on the ack, LIVE cue owns the acknowledged preview pixels, unreadable scene marks nothing; evidence in [the plan](docs/ui/operator-workspace-plan.md) |
| UI.3c acceptance (2026-10-31) | `workspace_ui_tests` thumbnail stepper (five steps, ends disabled, level written to the settings file), ask envelope (16:9 at the tile, 160 px floor, `thumbnailExtentFits` against the 640×360 slot), band cap/dedupe/coalescing over a 335-cue order (32 scenes exactly, 19 distinct keys across repeated entries, one ask at the destination), and presenter keys surviving both the clicks and the Edit→Show switch; `workspace_panels_tests` clamps `showThumbLevel` at both ends; evidence in [the plan](docs/ui/operator-workspace-plan.md) |
| Documentation | Current links, historical archive integrity and whitespace checked |

Presets ran sequentially. Frame/position progress was measured; acoustic offset,
long drift, projector hotplug and full-service soak were not measured here.
Those remain M7 acceptance work.

## Operator workspace

The fullscreen focus/minimize fix was confirmed by the user. The main workspace
uses the supplied ProPresenter palette/layout direction; Show is for presenting,
with slide authoring and destructive document shortcuts confined to Edit. The
sidebar outline/detail split and the running-order cue grid are in place (UI.2,
UI.3); the dedicated bottom media bin and right-hand control arrangement remain
UI follow-ups in [the reference](docs/reference.md).

## Evidence and next work

Keep PLAN.md updates to concise task summaries. Implementation and verification
details belong in the review documents; older detailed records remain in
[history](docs/history/README.md), including the unchanged [6.22g handoff](docs/history/PROGRESS-through-6.22g.md).
Next is UI.4a (media collections, cue browsing and type metadata), then UI.4b
and UI.5–6. M7 remains the release milestone.
