# Service workflow follow-ups — 2026-11-06

The operator's second report requested clearer drag feedback, title-only song
rows, Show-mode service reordering, fewer sidebar actions, the full service grid
on service selection and separator lines between service groups.

## Rollback checkpoints

- `944ec18`: Remove the Library's Add to service and Import song buttons and
  CCLI row labels. Titles use the reclaimed space; stored attribution remains.
  Toolbar More still opens clipboard import. The real-worker journey passes.
- `2a99fba`: Service selection now carries the stable entry/occurrence ID and
  returns the grid to the complete running order. It selects and scrolls to the
  occurrence's first slide without presenting. Library deck browsing remains
  separate. Authored presentations, songs, scripture and detached service
  copies all retain their complete service context, including repeated shows.
- `d37d102`: Draw a full-width 2px separator in the existing gap above each
  subsequent service group. No extra cells or cues are inserted, so cue mapping,
  presentation order, virtualization and thumbnail bands keep the same model.
- LRF.11: Enable service dragging in Show and Edit. A shared `DragFeedback`
  component displays the source title and add/move hint beside the pointer,
  outside list clipping. Valid service destinations gain a stronger border and
  a 4px insertion marker; category destinations highlight and say Move here.
  The badge changes add/move glyph and accepted-target colors to match the
  destination. Sources use open/closed-hand cursors; the dragged service row
  dims while its original slot remains visible.

The original service drag gate allowed gestures only in Edit. The new gesture
works from Show, reorders the selected occurrence identity rather than the show
identity, adjusts downward insertion gaps once, and leaves a no-op/cancelled
drop unchanged. Successful and same-gap drops select the dragged occurrence
for subsequent keyboard actions without adding an undo step for a no-op.
The current source delegate remains allocated during edge
scrolling; overlay coordinates keep the hotspot independent of list scrolling.

Qt's default MouseArea smoothing also discarded the initial threshold movement
from the drag target's position. Sources now set `drag.smoothed: false`, keeping
the drop hotspot aligned with the actual pointer. See the primary
[Qt MouseArea drag documentation](https://doc.qt.io/qt-6/qml-qtquick-mousearea.html#drag-prop).
The regression asserts pointer/hotspot alignment directly, including near the
left edge of the service target and while scrolling.

Accepted service drops return the grid from Library browsing to the full
service. Reordering is one ordinary undoable document edit and persists through
Save/reopen. Library category moves retain their existing durable path. The
remaining toolbar import route also preserves its originating page when opened
from Edit.

## Validation

The final desktop build (including the local executable) succeeds and all
**15 affected suites pass** (60.36 s). `git diff --check` is clean. The
real-worker drag journey also passes at 150% scale (9.52 s). Compact/full-size workspace,
separator and drag-overlay captures were visually reviewed.

The focused workspace and real-worker pointer journey pass **2/2** (46.21 s).
The expanded journey exercises actual pointer moves upward/downward among
repeated shows, same-gap no-op, invalid/outside release, edge autoscroll with the
source offscreen, insertion-marker cleanup, exact order, one-step undo and
Save/reopen. Existing workspace tests drive category moves both ways and
Library-presentation drops. No unexpected QML warnings or accidental presents
are allowed by the real-worker journey.

Reproduce the affected regression set after the desktop build:

```sh
CCACHE_DISABLE=1 cmake --build build --parallel 8
ctest --test-dir build --output-on-failure -R '^(domain_tests|catalog_tests|scene_tests|workspace_ui_tests|control_set_ui_tests|ui_loads|document_controller_tests|workspace_panels_tests|library_search_tests|library_removal_tests|catalog_controller_tests|song_import_tests|library_service_ui_tests|song_import_page_tests|song_service_tests)$'
```

Set `OPENSHOW_LIBRARY_CAPTURE=/tmp/openshow-followup-final` for populated
1000×680, 1380×800 and 1680×1020 frames, clipboard import and delete-confirmation
frames, and captures while adding/reordering (`-drag-add.png`, `-drag-move.png`).
These are UI and interaction evidence; physical audience-output release gates
remain separate.
