# OpenShowQt progress and handoff

Updated: 2026-11-01 (Europe/London).

## Current checkpoint

Current work: **the Themes amendment TH.7 is DONE** — a theme now carries the
PRIMARY lyric box's rectangle, so "the same look" means the same place too.
Placement resolves on the existing chain (slide placement customization →
current theme → slide theme → show default), drives the **first text item**
only, and is absent-by-default so no existing document or theme moves. The
surface leaf **TH.8** (Theme Editor placement canvas, Edit drag routing and
badge, captures and docs) is next.

Earlier in this milestone, **Themes TH.1–TH.6 are DONE**, verified
2026-11-01 with one pushed commit per leaf. Appearance is one Theme entity
(the former Style) in storage format 6 — readers still take formats 1–5 and
refuse files that mix the spellings — and every slide resolves per property
through customization → current theme → slide theme → show default →
built-in, compiled down to fully resolved styles so the wire and the worker
never learned the chain exists. The More ⋯ menu and the Theme Editor mode
(sample cue preview through the selected-scene lane, browsing never writes),
the Show header's viewed-show default selector, the flat cue and library
right-click menus (theme one slide, bulk-theme a presentation as one undo
step, restore customizations) and the Edit inspector (resolved values,
per-field revert badges, whole-slide restore, edits that customize the SLIDE
and can never restyle a shared theme) are the delivered surfaces; the Qt
6.12 shape deviations (flat menus, button-over-menu selector, focus-out is
not an edit) are recorded in
[the plan](docs/ui/themes-plan.md) and [the architecture](docs/architecture.md).
Gates 57/57 desktop and 43/43 headless; deterministic captures re-taken with
a show default, a worn slide theme and two live badges. **M7** (recovery,
budgets, packaging and service acceptance) is what remains in PLAN.md.

Previously: **UI.6 DONE**, verified 2026-10-31 — and with it every UI
leaf is closed. The header is the plan's row (Search · Show · Edit · Media
visibility left, unsaved/audience/Settings right; Settings is a mode at the
far edge, not a third page), Search is exactly Ctrl+F, the bin toggle
mirrors the bin's own state through the panel store, and keyboard handback,
drag cancellation and strip restore are pinned. Deterministic captures exist
at all three widths in both bin states plus the long-name, empty-collection
and disconnected states, and the reference differences are recorded rather
than glossed. Gates 57/57 and 32/32. The
[main-display plan](docs/ui/operator-workspace-plan.md)
holds the full record; **M7** (recovery, budgets, packaging and service
acceptance) is what remains in PLAN.md.

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
| Full headless suite with local GPU/PipeWire access | 32/32 PASS, 65.43 s; live-audio cases executed |
| Full desktop suite with local-session access | 57/57 PASS, 344.68 s; all 15 display-labelled checks |
| New focused regressions | Retirement claim race; deferred replacement/clear/cancellation with frontend preview pixels; removal during queued probes and shutdown; continued/paused seek playback; stage outcomes and transient notifications |
| UI.1 acceptance (2026-10-31) | `workspace_ui_tests` visible-tree audit (Show: no setup controls, only `librarySearchField` writable; Settings exposes routing), picker-URL regressions in `worker_controller_tests`/`document_controller_tests`, `ui_loads` + guards; evidence in [the plan](docs/ui/operator-workspace-plan.md) |
| UI.2 acceptance (2026-10-31) | `workspace_ui_tests` outline/detail interactions (duplicate entry ids, 180-char names, scrolling identities, drag onto a playlist row) and `document_present_tests` browsing-never-goes-live on the wire; evidence in [the plan](docs/ui/operator-workspace-plan.md) |
| UI.3a acceptance (2026-10-31) | `document_controller_tests` cue-group projection (duplicate shows, move/remove identity stability) and arbitrary-show compilation leaving the editing cursor fixed; evidence in [the plan](docs/ui/operator-workspace-plan.md) |
| UI.3b acceptance (2026-10-31) | `workspace_ui_tests` cue-grid projection (19 rows spanning two shows), click-selects-without-air, presenter-key boundary walk and browse-one-group-off-air; `frontend_journey_tests` §7 cue lane: pending synchronously, LIVE only on the ack, LIVE cue owns the acknowledged preview pixels, unreadable scene marks nothing; evidence in [the plan](docs/ui/operator-workspace-plan.md) |
| UI.3c acceptance (2026-10-31) | `workspace_ui_tests` thumbnail stepper (five steps, ends disabled, level written to the settings file), ask envelope (16:9 at the tile, 160 px floor, `thumbnailExtentFits` against the 640×360 slot), band cap/dedupe/coalescing over a 335-cue order (32 scenes exactly, 19 distinct keys across repeated entries, one ask at the destination), and presenter keys surviving both the clicks and the Edit→Show switch; `workspace_panels_tests` clamps `showThumbLevel` at both ends; evidence in [the plan](docs/ui/operator-workspace-plan.md) |
| UI.4a acceptance (2026-10-31) | `domain_tests` asset-kind table (kinds per name, `other`, one wire word, every extension in exactly one kind); `document_controller_tests` asset-row kinds, the import filter covering the table and not `.txt`, and kind surviving package adoption; `workspace_ui_tests` media bin (collections and counts, chips, notices, one-row filter, Remove, picture-only Add to slide placing on the current slide, narrow-row notice/name rule, collapse and edge drag preserving selection, navigation and monitor); `document_present_tests` proves import/remove leave the applied revision unmoved on the wire; evidence in [the plan](docs/ui/operator-workspace-plan.md) |
| UI.4b acceptance (2026-10-31) | `document_controller_tests` asset-row `exists` (absent name, a real file, and a rename that the row reports without any edit); `workspace_ui_tests` cue surface (Play present and disabled with no output, absent for a video row, a missing file named in both columns, the `noticeForState` table driven directly for live/missing/kind/refused, nothing put on the wire by browsing); `worker_controller_tests` refusal seam (arrives naming its path, survives the echoes either side, retired by the removal); `frontend_journey_tests` §8 on a real worker (AAC bed on air with its own clock, garbage refused naming the file while the bed carries on, a go moves none of it, removal is the stop); evidence in [the plan](docs/ui/operator-workspace-plan.md) |
| UI.5 acceptance (2026-10-31) | `workspace_ui_tests` rail section (clear strip beside the preview by scene position, all three clear controls disabled and saying "Clear all" with no output, transport nameless-empty with no controls, the audio combo holding audio names only — no video, no paths — picking sending nothing while Play/Stop stay disabled and the monitor keeps saying "Output disconnected", and no `playAudioButton` anywhere in the tree); `worker_controller_tests` transport scenario now confirms the three selection frames the operator can actually cause (the picker's URL half retired with the chooser); visible-tree audit on Show re-passes with the combo at all three capture sizes; evidence in [the plan](docs/ui/operator-workspace-plan.md) |
| UI.6 acceptance (2026-10-31) | `workspace_ui_tests` toolbar section (header offers Search, both operator pages, media visibility and Settings; the bin toggle closes the bin and re-opens with the bin's own bar agreeing both ways; toolbar Search takes focus to `librarySearchField`, Escape hands the keyboard back and the presenter's PgDn walk steps again; Settings/Show tab clicks switch modes and move the underline, and no header control hides on another page; a press released in place on a panel handle resizes nothing and a collapsed dock restores from its strip); deterministic captures at 1680/1380/1000 in both bin states, a one-row kind filter counting "1 shown", the fully-removed library ("no media — import a file" with the rail's "no prepared audio"), a long cue name whole at 1680 and elided narrower, and the disconnected bar/rail — all inspected, with the remaining reference differences recorded in [the plan](docs/ui/operator-workspace-plan.md) |
| Themes TH.1–TH.6 acceptance (2026-11-01) | `domain_tests` format-6 round-trip, 1–5 migration and mix-spelling refusals, and the per-property precedence matrix across customization/current/slide/show/built-in with the answered empties; `scene_tests`, `document_present_tests` and `frontend_journey_tests` re-skin to acknowledged pixels through the chain; `document_controller_tests` exercises the whole TH.5 surface API over real fixtures (cross-show theming, bulk-undo-once, refusal statuses); `workspace_ui_tests` drives the shipped UI (header menu names the viewed show's default and reopens checked, flat cue/library menus theme and restore by real clicks, badges appear per customized field and revert only their own, refusing values snap back with the domain's wording, and a page switch commits nothing); captures at 1680/1380/1000 inspected themed — Loud as the header default, the edited slide wearing House with exactly the flat-gradient and size-44 badges; 57/57 desktop and 43/43 headless; evidence in [the plan](docs/ui/themes-plan.md) and [the architecture](docs/architecture.md) |
| Themes TH.7 acceptance (2026-11-01) | `domain_tests` placement matrix — a theme without a rectangle is silent (never zero-sized), customization beats current beats slide beats show beats authored, one revert unit, undo, ghost-free theme deletion — plus format-6 round-trip (byte-stable, absence stays absence) and refusals for a non-object, a missing corner, a zero extent and a hand-bumped key in a format-5 file; `scene_tests` prove the themed rectangle drives the FIRST TEXT item only: a picture stacked on top and every additional box keep their authored geometry, and a picture-only slide gets no invented box; 57/57 desktop and 43/43 headless |
| Documentation | Current links, historical archive integrity and whitespace checked |

Presets ran sequentially. Frame/position progress was measured; acoustic offset,
long drift, projector hotplug and full-service soak were not measured here.
Those remain M7 acceptance work.

## Operator workspace

The fullscreen focus/minimize fix was confirmed by the user. The main workspace
uses the supplied ProPresenter palette/layout direction; Show is for presenting,
with slide authoring and destructive document shortcuts confined to Edit. The
sidebar outline/detail split, the running-order cue grid, the bottom media
bin — browsing and its cue activation — the rail's monitor/clears/transport/
audio arrangement and the toolbar with its three-size visual acceptance are
in place (UI.2, UI.3, UI.4, UI.5, UI.6); the plan's remaining reference
differences are the recorded toolbar proportions listed in
[the plan](docs/ui/operator-workspace-plan.md).

## Evidence and next work

Keep PLAN.md updates to concise task summaries. Implementation and verification
details belong in the review documents; older detailed records remain in
[history](docs/history/README.md), including the unchanged [6.22g handoff](docs/history/PROGRESS-through-6.22g.md).
The UI workspace plan (UI.1–UI.6) is complete. M7 (recovery, budgets,
packaging and service acceptance) remains the release milestone.

**Themes (TH.1–TH.6) DONE 2026-11-01**, one pushed commit per leaf, closing
the ledger in [docs/ui/themes-plan.md](docs/ui/themes-plan.md): Style promoted
to Theme with storage format 6 and the format-5 migration (TH.1), current-theme
and sparse per-slide customizations resolving the full chain (TH.2), the compile
and controller following the chain to acknowledged pixels (TH.3), the More ⋯
menu and Theme Editor mode (TH.4), the apply/restore surfaces and the
customize-not-restyle Edit inspector (TH.5), and the themed captures with the
architecture write-up (TH.6). The model decision the operator confirmed —
customization > current theme > slide theme > show default > built-in,
appearance-only themes (template slides deferred) — is what shipped.

**Themes amendment TH.7 DONE 2026-11-01**: the operator found appearance-only
half an answer, so a theme carries one `placement` rectangle for the PRIMARY
lyric box, resolved on the same chain and compiled onto the slide's first text
item only. Storage stays format 6 — `placement` is an optional member of a
theme entry and of a slide's customizations, and pre-6 files that carry the
key are refused. **TH.8**, the surfaces (Theme Editor placement canvas, Edit
drag routing and badge, captures, docs), is next; **M7** follows.
