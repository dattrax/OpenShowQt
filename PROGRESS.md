# OpenShowQt progress and handoff

Updated: 2026-10-31 (Europe/London).

## Current checkpoint

Current UI work: **UI.4b DONE**, verified 2026-10-31 (the media bin's one cue
control — Play/Stop on an audio row, which is the worker's independent
production track and nothing else, while the kinds with no runtime of their
own are told where they go; the worker's probe is the only thing that may say
a file plays, so a lost file reads "file is not here" and a refused probe
marks its row "cannot play" naming the file on Show and quoting the worker in
Edit; gates 57/57 and 32/32). UI.4 is closed. The
[main-display plan](docs/ui/operator-workspace-plan.md)
audits every current control and defines the remaining leaves; the next leaf is
**UI.5** (runtime monitor and show controls: the right rail's clear controls,
transport, and a prepared-audio area replacing the interim file chooser).

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
| Full headless suite with local GPU/PipeWire access | 32/32 PASS, 65.71 s; live-audio cases executed |
| Full desktop suite with local-session access | 57/57 PASS, 346.07 s; all 15 display-labelled checks |
| New focused regressions | Retirement claim race; deferred replacement/clear/cancellation with frontend preview pixels; removal during queued probes and shutdown; continued/paused seek playback; stage outcomes and transient notifications |
| UI.1 acceptance (2026-10-31) | `workspace_ui_tests` visible-tree audit (Show: no setup controls, only `librarySearchField` writable; Settings exposes routing), picker-URL regressions in `worker_controller_tests`/`document_controller_tests`, `ui_loads` + guards; evidence in [the plan](docs/ui/operator-workspace-plan.md) |
| UI.2 acceptance (2026-10-31) | `workspace_ui_tests` outline/detail interactions (duplicate entry ids, 180-char names, scrolling identities, drag onto a playlist row) and `document_present_tests` browsing-never-goes-live on the wire; evidence in [the plan](docs/ui/operator-workspace-plan.md) |
| UI.3a acceptance (2026-10-31) | `document_controller_tests` cue-group projection (duplicate shows, move/remove identity stability) and arbitrary-show compilation leaving the editing cursor fixed; evidence in [the plan](docs/ui/operator-workspace-plan.md) |
| UI.3b acceptance (2026-10-31) | `workspace_ui_tests` cue-grid projection (19 rows spanning two shows), click-selects-without-air, presenter-key boundary walk and browse-one-group-off-air; `frontend_journey_tests` §7 cue lane: pending synchronously, LIVE only on the ack, LIVE cue owns the acknowledged preview pixels, unreadable scene marks nothing; evidence in [the plan](docs/ui/operator-workspace-plan.md) |
| UI.3c acceptance (2026-10-31) | `workspace_ui_tests` thumbnail stepper (five steps, ends disabled, level written to the settings file), ask envelope (16:9 at the tile, 160 px floor, `thumbnailExtentFits` against the 640×360 slot), band cap/dedupe/coalescing over a 335-cue order (32 scenes exactly, 19 distinct keys across repeated entries, one ask at the destination), and presenter keys surviving both the clicks and the Edit→Show switch; `workspace_panels_tests` clamps `showThumbLevel` at both ends; evidence in [the plan](docs/ui/operator-workspace-plan.md) |
| UI.4a acceptance (2026-10-31) | `domain_tests` asset-kind table (kinds per name, `other`, one wire word, every extension in exactly one kind); `document_controller_tests` asset-row kinds, the import filter covering the table and not `.txt`, and kind surviving package adoption; `workspace_ui_tests` media bin (collections and counts, chips, notices, one-row filter, Remove, picture-only Add to slide placing on the current slide, narrow-row notice/name rule, collapse and edge drag preserving selection, navigation and monitor); `document_present_tests` proves import/remove leave the applied revision unmoved on the wire; evidence in [the plan](docs/ui/operator-workspace-plan.md) |
| UI.4b acceptance (2026-10-31) | `document_controller_tests` asset-row `exists` (absent name, a real file, and a rename that the row reports without any edit); `workspace_ui_tests` cue surface (Play present and disabled with no output, absent for a video row, a missing file named in both columns, the `noticeForState` table driven directly for live/missing/kind/refused, nothing put on the wire by browsing); `worker_controller_tests` refusal seam (arrives naming its path, survives the echoes either side, retired by the removal); `frontend_journey_tests` §8 on a real worker (AAC bed on air with its own clock, garbage refused naming the file while the bed carries on, a go moves none of it, removal is the stop); evidence in [the plan](docs/ui/operator-workspace-plan.md) |
| Documentation | Current links, historical archive integrity and whitespace checked |

Presets ran sequentially. Frame/position progress was measured; acoustic offset,
long drift, projector hotplug and full-service soak were not measured here.
Those remain M7 acceptance work.

## Operator workspace

The fullscreen focus/minimize fix was confirmed by the user. The main workspace
uses the supplied ProPresenter palette/layout direction; Show is for presenting,
with slide authoring and destructive document shortcuts confined to Edit. The
sidebar outline/detail split, the running-order cue grid and the bottom media
bin — browsing and its cue activation — are in place (UI.2, UI.3, UI.4); the
right-hand control arrangement remains a UI follow-up in
[the reference](docs/reference.md).

## Evidence and next work

Keep PLAN.md updates to concise task summaries. Implementation and verification
details belong in the review documents; older detailed records remain in
[history](docs/history/README.md), including the unchanged [6.22g handoff](docs/history/PROGRESS-through-6.22g.md).
Next is UI.5 (runtime monitor and show controls in the right rail, with the
prepared-audio area replacing the interim file chooser), then UI.6. M7 remains
the release milestone.
