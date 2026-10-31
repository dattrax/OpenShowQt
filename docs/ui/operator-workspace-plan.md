# Operator workspace: main-display plan

Updated 2026-10-31. Scope: the Qt operator window and its Show view.

The [Renewed Vision interface guide](https://support.renewedvision.com/hc/en-us/articles/360041345954-Understanding-The-ProPresenter-User-Interface#h_01K2N9HA49M3GT5NJ9N8777FCY)
and `third_party/cache/propresenter-visuals/` are the layout references.
The guide distinguishes a left collection outline/detail area, central slides,
bottom media, and right-side preview, clears, transport and show controls.

## Product boundary

Show runs the service: find a presentation, trigger a cue, see what is live,
control playback and clear/restore content. Edit authors slide content. Settings
configures the installation and audience destination. Merely browsing a library,
opening Settings, choosing a preview or resizing a panel must never present a cue
or move the audience window.

The existing worker, document/history owner, acknowledged presentation model and
worker-rendered previews remain authoritative. This work changes their frontend
presentation; it does not redesign the audience renderer or expand the deferred
stage, scripture, streaming, cloud or multi-output scope.

## Audit and placement decisions

| Existing item | Decision for Show | Owner / replacement |
|---|---|---|
| Audience display routing / Windowed picker | Remove | Settings → Audience display; explicit selection moves the output and follows applied status |
| Background/images/words routing switches | Remove | Settings → Audience content, with user-facing labels |
| Raw aux/clock renderer switches | Remove | No public controls until corresponding product features exist |
| Audience preview and current destination name | Keep, read-only | Right monitor; this reports the destination and does not configure it |
| Clear slide/background/all, restore | Keep | Runtime cues; later arrange beside the preview |
| Present / Previous / Next / Follow selection | Keep as interim runtime controls | UI.3b replaces implicit follow-arming with consistent Show triggers |
| Play/pause, seek, loop, gain and mute | Keep when applicable | Transport and audio show controls, bound to worker acknowledgements |
| Typed audio path / Set track form | Remove | Explicit Play audio file chooser now; prepared audio bin in UI.5 |
| Active audio filename, elapsed time and Stop | Keep | Runtime audio state; no absolute path form |
| Protocol versions, worker/GPU diagnostics and service directory | Remove | Settings → Service and diagnostics; Show gets concise readiness/attention labels |
| Media failures | Keep concise, actionable notice | Filename on Show; detailed failure text/path in Settings |
| Add/remove/edit slides, text, styles and geometry | Exclude | Edit only, including mutating shortcuts |
| Library/media import | Keep as preparation | Registers an asset without presenting it; native file URL conversion belongs in C++ |
| Remove library assets / add asset to authored slide | Exclude | Edit; importing must not implicitly modify a slide |
| Full media source paths in list/details | Exclude | Edit details; Show identifies items by name |
| Duplicate Shows library in bottom drawer | Replace in UI.4 | Presentation browsing belongs in the left sidebar; bottom becomes media |
| Playlist chips above a second library list | Replace in UI.2 | One collection outline and one selected-collection detail view |
| Inactive/future feature buttons | Exclude | Expose a control only when its complete action exists |

Disabled controls for an unavailable output or absent current cue are valid
supported states; unimplemented feature controls stay absent.

The first cleanup changes only these ownership boundaries. It does not claim
that the remaining hierarchy or styling already matches the reference.

## Target structure

```text
┌ Search · Show · Edit · Media visibility ─── Audience state · Settings ┐
├ Collection outline ┬ Presentation headings + slide grid ┬ Preview  ┬┤
│ Libraries          │                                    │ audience ││ clears
│ Playlists          │ selected and live cues distinguished├──────────┤│
├ Selected contents  │                                    │ transport │
│ Presentations      ├ View controls + thumbnail size ─────┼───────────┤
├ Media collections ─┼ Media thumbnails / filter ──────────┤ audio     │
└────────────────────┴────────────────────────────────────┴───────────┘
```

Use neutral charcoal surfaces, modest separators, compact controls and blue
selection. Reserve a distinct acknowledgement-backed live marker. Keep the main
slide area visually dominant; the audience monitor remains visible when the
media bin opens. Settings is a separate mode, not an expandable setup form in
the live-output dock.

A future preview-target picker may change what is inspected only. It must not
reuse `worker.selectDisplay()`, which changes physical routing. With one audience
output, a destination readout is sufficient. Likewise, Clear all is a content
operation; it must not be relabelled as an output-enable switch.

## Work order and acceptance

### UI.1 — Establish the Show boundary — CLOSED 2026-10-31

Move audience configuration to Settings, replace the audio-path form, remove
normal protocol/path diagnostics from Show, and make Settings scroll correctly.
Preserve concise failure feedback and the existing runtime controls. Correct
file-picker URL handling before leaving import/play actions exposed.

Acceptance: an actual visible-tree check finds no routing/content-setup controls
or writable fields other than library search in Show. Settings contains the
configuration controls. Local filenames containing spaces, `#` and Unicode reach
the controller correctly; non-local URLs cannot alter the current track/library.
The shell loads without QML errors and remains usable at the three capture sizes.

Verification (2026-10-31): the boundary shipped in `5ad557c`/`6051572`.
`workspace_ui_tests` walks the live visible tree on Show (destination picker
hidden, no content-layer settings, the only writable field is
`librarySearchField` — audited with the drawer open and with search focused),
shows the routing picker on Settings, keeps the text editor off Show, and
re-audits all three capture sizes with zero QML warnings.
`worker_controller_tests` selects a picker URL `/tmp/Walk in #1 — café.m4a` and
reads the decoded local path on the wire, then an `https:` URL which is refused
with the live track and status attention left correct;
`document_controller_tests` registers `loop #1.mp4` by decoded name/path and
refuses a non-local URL without creating a library entry. `ui_loads` plus its
two guard negatives pass. Gates: desktop 57/57 (338.55 s, 15 display-labelled),
headless 32/32 (65.92 s); both trees build clean.

### UI.2 — One sidebar outline and one detail view — CLOSED 2026-10-31

Replace the flat library plus horizontal playlist chips with an outline of the
existing library and playlists. Selecting an outline item controls the detail
list beneath it. Detail rows show presentation names, sections and counts, with
clear selection and a useful empty state. Keep preparation actions separate from
live slide triggers; never duplicate a second song list in this sidebar.

Acceptance: library/playlist selection, duplicate playlist entries, long names,
scrolling and supported drag/drop preserve stable identities. Selecting or moving
a presentation never goes live. Add read-only projections through DocumentViews;
do not introduce another document or history owner.

Verification (2026-10-31): `PlaylistPanel.qml` is now one outline
(`collectionOutline`) above one detail view (`collectionLibraryList` /
`collectionPlaylistList`); the flat library box and horizontal playlist chips
are gone and no second song list exists. The row shape keeps one author:
`DocumentViews::collectionOutline()` — Library first, then every playlist with
its entry count — exposed as the controller's `collectionOutline` property;
the panel selects through it and never mutates through it. `workspace_ui_tests`
walks the live tree: the Library leads the outline, selecting it moves only
the detail view, a 180-character name stays one bounded selectable row, the
same show twice in an order carries two distinct entry ids of which exactly
one wears the click selection, a fourteen-cue order scrolls with every visible
row still naming its own cue, and dragging a Library row onto a playlist
outline row prepares that show at the order's end while the viewed order and
selection stay coherent. `document_present_tests` proves the never-goes-live
half on the wire: with a scene on air, creating presentations and playlists,
duplicating, inserting and moving entries and clicking selections leaves the
worker's active revision unchanged across repeated status polls while the
edited document has visibly moved on. Preparing (add/insert/move/drop) and
triggering remain separate paths; `+` and the reorder handles stay behind
`editingEnabled`. Gates: desktop 57/57 (339.21 s, 15 display), headless
32/32 (66.02 s); both trees build clean.

### UI.3 — Presentation-oriented slide workspace

Implement independently: **UI.3a** provides the read-only cue projection and
arbitrary-show compilation; **UI.3b** defines triggers, navigation and live markers;
**UI.3c** adds size/view controls and bounded thumbnail scheduling.

Render the chosen presentation or running order as presentation headings and
slide groups. Use consistent Show behaviour: clicking a slide presents it;
browsing a collection/presentation does not. Remove implicit follow-arming state.
Keep selection and confirmed live state distinct, including pending/failed go.
Next/previous walks the chosen running order; Edit navigation remains off air.
Add thumbnail-size control; expose additional view modes only as they work.

Acceptance: arbitrary-show scene compilation and thumbnail requests do not move
the editing cursor. Navigation covers presentation boundaries and repeated
playlist entries. Visible-band requests remain bounded by the existing 32-cue
limit; zoom/scroll coalesce work and retain the destination/extent guarantees.
A real worker test checks the live marker against acknowledged content and pixels.

Verification (UI.3a, 2026-10-31): `DocumentViews::runningOrderCues()` authors
the cue groups — one per playlist entry `[{entryId, showId, showName, section,
entryIndex, slides}]` — exposed as `DocumentController::runningOrderCues` over
the playlist under preparation; a show listed twice is two cues with two entry
ids whose groups list the same slide rows, and a moved or neighbour-removed cue
keeps its entry id. `document_controller_tests` pins that projection and the
cursor rule: with show B selected, compiling every cue of every group succeeds
from the projection alone while `currentSongId`/`currentSlide` do not move —
paid for in `compileSlideJson`, which now resolves the slide's OWNING show
(current show first) instead of silently failing on any other. Gates: desktop
57/57 (339.83 s, 15 display), headless 32/32 (65.92 s); both trees build clean.
UI.3b and UI.3c remain TODO.

### UI.4 — Dedicated media bin

Implement independently: **UI.4a** provides collection/cue browsing and trustworthy
type metadata; **UI.4b** connects explicit runtime activation. Asset preparation
must keep file probing off the UI thread, with the worker validating playback.

Replace the bottom Shows/Media drawer with a media area: collections/filters on
the left and named image/video cues in the content area. Put library search in
the toolbar/sidebar. Keep import separate from cue activation and from placing
media onto an authored slide. Add grid/list and size controls only with real
models/actions; omit unsupported collections and input sources.

Acceptance: filtering and importing never present or rewrite a slide. Cue
activation follows the actual media type and worker validation. Empty, missing,
corrupt and renamed assets have useful names/notices. Closing or resizing the bin
preserves sidebar selection, slide navigation and the audience monitor.

### UI.5 — Runtime monitor and show controls

Arrange compact clear controls beside the preview, playback beneath it and an
audio area below. Offer supported clear operations with honest enablement;
unsupported message/prop/stage/timer/macro controls stay absent. Populate the
audio area from prepared assets with explicit cue activation and Stop, replacing
the interim direct file chooser. Use native asset metadata/type validation rather
than guessing audio eligibility from file extensions.

Acceptance: live preview is always distinct from selection previews. Clears,
restore, seek, loop, gain, mute and audio selection continue using acknowledged
state. No monitor-routing or renderer-layer setup reappears. Independent audio
survives slide/background changes, using the existing transport contracts.

### UI.6 — Toolbar, layout and visual acceptance

Add working Search and media-bin visibility controls. Keep Show/Edit/Settings
navigation, a concise audience state and actionable attention feedback. Inspect
every visible label/control against the allowlist above. Use responsive layouts
and scrolling so controls do not overlap, disappear on hover or collapse merely
because a different page is selected.

Acceptance: deterministic populated captures at 1000×680, 1380×800 and 1680×1020,
with both bin states, long names, empty collections and disconnected/faulted
output. Verify keyboard ownership after search/dialogs, drag cancellation and
panel restore. Compare hierarchy, proportions, spacing, contrast and live/selected
markers against the reference image; record remaining differences before claiming
completion. Run workspace/controller tests and relevant live frontend journeys.

## Completion rule

UI.1, UI.2 and UI.3a are closed (verification recorded above); UI.3b–6 remain TODO
until implemented and checked.
A colour change or one screenshot does not close the layout work. Keep the short
ledger in PLAN.md; store audit details and later capture evidence alongside this
plan. M6's completed async fixes remain DONE, and M7 release gates remain open.
