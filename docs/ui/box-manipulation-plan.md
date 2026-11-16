# Direct box manipulation — 2026-11-16

Implementation brief for **BX** in `PLAN.md`, following the delivered TL group.
The operator wants to place and size boxes visually: all displayed resize handles
must work, and the numeric geometry section in the supplied screenshot goes away.
This document plans the work; no interaction fix is implemented by this update.

## Operator contract

- In Theme Editor's Songs and Bible views and on Edit, drag a box's body to move
  it and drag any of its four visible corner handles to resize it. Keep four
  corners; adding edge-midpoint handles is outside this task.
- Each corner works horizontally and vertically, inward and outward. The
  opposite corner stays fixed. Body movement preserves width and height.
- Moving a box near either slide centre snaps that axis exactly to centre and
  shows its guide. Preserve the current six logical-screen-pixel tolerance and
  independent horizontal/vertical snapping. A move outside that tolerance stays
  free; guides disappear when the gesture ends or is cancelled.
- Remove the whole geometry inspector block: its X/Y/W/H fields, authored-pixel
  heading, Center H/Center V buttons, placement-owner sentence and No placement/
  No box action row. Remove the corresponding numeric geometry band from Edit.
  Do not put the fields into a new Advanced section or show coordinate readouts
  on the canvas. Internal authored geometry remains unchanged.
- This removal does **not** remove TL's horizontal/vertical text alignment,
  All caps, the box's text/style controls or Edit's add/delete/reorder commands.
- Preserve placement inheritance through a **Reset box placement** action on the
  selected box's context menu, reachable by right-click and Menu/Shift+F10.
  Theme targets remove their own placement; Bible removes its box/inset override
  while preserving other view fields; Edit's primary box removes its placement
  customization and follows the theme. Enable it only when an applicable own
  placement exists. Secondary authored items retain their normal undo path.
  Keep Bible's existing borrow-the-theme-box action in this menu as **Use theme's
  box**, available only for the Bible view; it must not reintroduce numeric UI.

The underlying theme/context/slide/item ownership stays as today. Theme editing
sets its selected view's default, Edit's primary box writes a slide placement
override, and secondary text/image items edit their own authored rectangle.
No new document format, wire schema, per-box typography or output command is
needed. Saved numeric coordinates remain for accurate rendering and round-trip.

## Findings to correct

Both `EditCanvas.applyDrag()` and `ThemeEditorPage.moveGesture()` compute only
the dragged **left** and **top** edges, then derive size from the old right and
bottom. A bottom-right drag therefore leaves the rectangle unchanged; top-right
only moves the top edge, and bottom-left only moves the left edge. The same
partial implementation is duplicated in both pages.

Both canvases paint handles centred on the rectangle corners, but their broad
MouseArea is limited to the fitted slide. At a slide boundary, the visible
handle extends beyond that input surface. Coverage must include the exposed
handle area, not merely the half that happens to lie inside the slide.

ThemeEditor's MouseArea currently routes `onCanceled` to `endGesture()`, which
commits. Cancellation must instead discard the transient rectangle. Existing
tests check handle positions and invoke snap helpers, but do not prove real
pointer resizing through every corner, so the regression can remain green.

## Shared interaction and geometry

Use one shared box-transform overlay/helper for corner placement, hit testing,
move/resize calculations and centre guides; both editors must consume it.
Keep their existing target selection, stacking and document mutation verbs in
their adapters. Do not create another document/history owner.

- At press, capture the target's stable identity, starting rectangle and pointer
  origin. Identity is theme + view, or show + slide + item ID, rather than a
  current list index that can move before release.
- Derive coordinates from the exact fitted preview content rectangle with one
  screen-to-authored transform. Letterboxing, window size, zoom and display
  scale must affect chrome, hit testing and drag distance identically.
- Resolve selected handles before body hits. A handle retains its selected box
  even if its corner overlaps another item; a body press still selects the
  topmost item in the authored stack. Give handles the correct diagonal resize
  cursor and the body a move cursor. Use at least a 16×16 logical-pixel hit
  target around each visible corner, independent of authored zoom. Where hit
  regions overlap, choose the nearest handle centre deterministically.
- Keep hit areas available on all slide edges, including the handle's visible
  outer half, without covering the inspector or swallowing unrelated controls.
  Tiny legacy rectangles must remain selectable; preserve existing keyboard
  move/resize for accessibility and recovery when handles are close together.
- For TL/TR/BL/BR resize, update the two appropriate edges from the captured
  rectangle. Clamp each moving edge against its fixed opposite edge and canvas
  bounds, retaining the existing 20-authored-unit gesture minimum where the
  fixed anchor permits it. Do not flip the item or change its stack order.
  Moving/inspecting an old smaller rectangle must not normalize it merely on
  selection; an unchanged release writes nothing.
- Moving a valid in-canvas rectangle stays within the canvas and uses the one
  shared centre-snap rule. Resize does not introduce a new snapping policy.
  Stored off-canvas legacy geometry must load unchanged; a corrective drag
  yields a valid rectangle or is refused cleanly by the existing domain.
- During a gesture, update the transient outline/handles immediately. Commit
  one rectangle on release through the original target's existing verb; the
  worker preview then refreshes through the normal document-change path.
  Keep worker-rendered text; do not simulate wrapping with QML.
- No movement means no undo entry. Escape, pointer cancellation, page/view/
  target change or deletion cancels without a write, restores model geometry
  and clears guides/capture. Never apply a held drag to a newly selected target.
  A refused commit also resynchronises from the model.
- A real completed gesture is one undo step, restoring exactly with undo/redo.
  Box edits and reset actions affect the off-air preview; audience content still
  changes only on explicit Go. Retain Edit's existing keyboard nudges/resizing
  and stack tools after removing the numeric band.

## Geometry-section cleanup and reset access

Remove live `BoxGeometryControls` consumers and host-specific configuration/
sync hooks when replacing the section. Retire the component and its build/QML
registration if no consumers remain. Edit's `boxFields` currently combines
numeric geometry with placement reset and stack commands: remove the numeric
and ownership rows, move reset into the box menu and preserve the stack tools
and their anchoring. Do not delete the entire container blindly.

The context menu takes the same stable target identity as the drag. Its reset
and Bible borrow actions use `InspectorAdapter`/existing domain verbs, preserve
typography, vertical alignment, All caps, effects, transition and other fields,
and create at most one undo step. Merely opening a menu or selecting a box must
not materialise a theme/context placement or add history.

Update the shared control-set catalogue and current architecture description.
Keep earlier DONE evidence historical; do not claim that obsolete numeric UI
tests establish the new direct-manipulation contract.

## Acceptance

The implementation ledger is **BX.1–BX.4** in `PLAN.md`. BX.1 (the shared
`BoxTransform` helper and corrected four-corner resize) and BX.2 (reachable
handle areas and identity-owned gesture lifetime) are implemented; the rest
remain TODO.

- Exercise real mouse press/move/release on each corner in Theme Songs, Theme
  Bible and Edit, in both directions. Check the moving edges, fixed opposite
  corner, resulting width/height and one-undo round-trip. Bottom-right must
  change both dimensions; top-right and bottom-left must change both intended
  edges. These assertions must fail against the reviewed implementation.
- Use visible handles at every slide boundary, overlapping text/image boxes,
  small rectangles, and fitted canvases at widths 1000/1380/1680 with display
  scales 1/1.5/2. Verify correct cursors, target identity and no clipped/dead
  handles. Mathematical helper tests supplement real-pointer tests.
- Drag the body onto horizontal centre, vertical centre and their intersection;
  assert exact snapped geometry and guide visibility. Drag beyond the tolerance
  and confirm unsnapped placement. Centre movement must preserve size.
- Cancellation, click-without-motion, rejected geometry, and navigation/view
  changes during a held drag write nothing. The next gesture remains usable.
  Selected-handle overlap must not edit the item underneath it.
- Assert absence of numeric headings/fields, Center H/V, ownership sentences and
  the old placement action row in all three hosts. Verify no blank spacer where
  the removed section sat. TL's alignment/caps and Edit's stack/keyboard tools
  remain reachable.
- Exercise context-menu reset/borrow by mouse and keyboard: remove only the
  intended placement, preserve sibling style/view fields, undo once and
  save/reopen. Verify browsing/opening menus leaves inherited placement absent.
- Finish a real-worker journey: resize/move → matching off-air box/wrapping →
  explicit Go → acknowledged audience pixels. Authored IDs/content, layer order
  and media continuity survive the edit and save/reopen.

Run focused workspace/control-set/panel/controller and output-journey regressions,
then desktop/headless presets and source guards. Record skips, populated captures
and physical checks honestly; none are claimed by this planning update.
