# Edit slide navigation — 2026-11-06

The operator asked for clickable slide pictures in Edit, replacing the
Library/service outline and top slide picker. They clarified that the rail
must show **only the selected song/presentation**, rather than the full service.
They also requested removal of the Save/Load/Undo toolbar controls and Delete
as slide removal.

## Delivered behavior

- `15c8fcd`: Remove the top Remove slide, Undo/Redo and Save/Load buttons.
  Delete removes the current slide as one ordinary undoable edit. Text inputs
  retain native Delete, Show does not acquire slide deletion, and empty/final
  slide cases remain safe. Ctrl+Z/Ctrl+Y, Ctrl+S and recovery autosave remain.
  The user approved keeping the concurrently staged PowerPoint submodule
  additions which were included in that pushed commit.
- `EditorSlideRail` replaces the Library/service pane in Edit with a 220px
  navigation rail, matching the Themes navigation width. The selected
  presentation supplies its own ordered slide rows, including service-only
  songs and imported presentations. The header names the presentation; each
  numbered card displays a real worker thumbnail and current-slide highlight.
- Clicking a thumbnail flushes pending text to the slide being left, selects
  the requested slide and updates the editing canvas without presenting.
  Up/Down/Home/End navigate and reveal slides while the rail has focus; they
  cannot nudge an authored box. Delete and keyboard undo also work from the rail.
- Remove the old combo picker and remaining Add-slide toolbar row. Add slide
  moves to the rail footer; New presentation remains available from More when
  Library is hidden. Ctrl+F returns to Show's Library search.

The rail uses the existing `SlidePreviewCard`/`SlidePreviewSurface` and bounded
worker thumbnail window. Only the visible rows plus one look-ahead are compiled
and requested after a coalescing timer; selected slides remain reachable in long
lists. Document edits refresh the cards, presentation changes replace the model,
and hiding Edit cancels its requests and relinquishes the thumbnail lane. The
canvas retains the separate Selection target. There is no new scene renderer,
persistence owner, document format or presentation path.

## Validation

The final affected-target build succeeds. All **16 affected suites pass**
(65.03 s), and the real-worker pointer journey passes at **150% scaling**
(11.57 s). The selected card remains fully visible after three window resizes.
Populated Edit captures at three sizes/scales 1 and 1.5 were reviewed; the top
actions are gone and the chosen presentation has real thumbnail pictures.
`git diff --check` is clean.

The focused workspace and real-worker journeys pass 2/2 (46.63 s). Their new
coverage verifies selected-show-only rows and model replacement, actual thumbnail
clicks, pending-text commit ownership, keyboard navigation without geometry
edits, last-slide scrolling, Add slide, native text Delete, one-step removal
undo, empty/final-slide behavior, actual Thumbnail and Selection frames,
hidden-page silence and no audience presents or unexpected QML warnings.
The workspace also verifies New presentation is reachable from More with the
Library hidden.

Reproduce the affected acceptance set after rebuilding the UI/test targets:

```sh
ctest --test-dir build --output-on-failure -R '^(domain_tests|catalog_tests|scene_tests|workspace_ui_tests|control_set_ui_tests|ui_loads|document_controller_tests|workspace_panels_tests|library_search_tests|library_removal_tests|catalog_controller_tests|song_import_tests|library_service_ui_tests|song_import_page_tests|song_service_tests|inspector_adapter_tests)$'
```

`OPENSHOW_LIBRARY_CAPTURE=/tmp/openshow-edit-final` saves the existing journey's
frames plus Edit at 1000×680, 1380×800 and 1680×1020 with Media collapsed.
`QT_SCALE_FACTOR=1.5` repeats the pointer/renderer journey at fractional scaling.
The canvas/inspector keep their existing scrollable layout at smaller widths.
Physical audience-output release gates remain separate from this UI acceptance.
