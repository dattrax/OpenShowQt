# Library operator fixes — 2026-11-06

The operator requested immediate fixes for clipboard-import access, search
motion/redundant toolbar space, and bulk removal in both preparation lists.

## Delivered behavior

- **LRF.1 (`6306f72`):** Show exposes Import song directly in Library; More has
  the same entry. Paste, measure and Add use the existing import page and return
  to the originating page. Tests now use those visible actions.
- **LRF.2 (`be48b69`):** Library search uses a stationary hint; the toolbar Search
  action is removed. Ctrl+F remains and actual drag/drop preserves the query.
- **LRF.3 (`2f8b80c`):** Ctrl toggles service entries, Shift selects ranges,
  Ctrl+A selects the running order, and Delete/Remove removes selected entry
  occurrences in one validated document edit. Repeated songs keep separate IDs.
- **LRF.4:** Library uses the same `ListSelection` part, with typed catalog/show
  IDs across Songs and Presentations. Ctrl+A includes scrolled-off filtered rows
  in expanded branches; filtering/collapsing discards hidden selections. Delete
  and Remove share one command path. Text inputs retain native editing keys.

Catalog batches validate and commit through the existing trial-copy/durable-write
path before notifications. Failed/stale batches preserve rows and file bytes.
Removing authored Library presentations hides referenced shows using the existing
`scheduleOwned` flag, or deletes unreferenced shows. Catalog removal detaches
existing service copies; those copies stay playable and do not reappear in
Presentations. No stored-format changes or autosave migration are involved.

Ctrl+Z/Ctrl+Y/Ctrl+Shift+Z work from a focused preparation list on Show and Edit.
A mixed Library removal uses a history step in each owning store, and its
immediate undo/redo visits both. There is no cross-file transaction or restoration
of a previously detached catalog link. Service insertion hands focus to its
destination so its next Undo acts on the service.

Compact buttons had too little content height to paint their labels. Vertical
padding and action widths now leave room for the text. The open Media drawer
leaves 400 px for preparation, enough for full category rows and a separately
scrollable service list even at 1000×680.

## Validation

`CCACHE_DISABLE=1 cmake --build build --parallel 8` succeeds, including the actual
`src/ui/openshow-ui` executable. Fifteen affected suites pass:

`domain_tests`, `catalog_tests`, `scene_tests`, `workspace_ui_tests`,
`control_set_ui_tests`, `ui_loads`, `document_controller_tests`,
`workspace_panels_tests`, `library_search_tests`, `library_removal_tests`,
`catalog_controller_tests`, `song_import_tests`, `library_service_ui_tests`,
`song_import_page_tests`, `song_service_tests`.

These cover stale/duplicate IDs, rollback and durable-write failure, service and
catalog restart, batch undo/redo, native search Delete, filtered Ctrl+A, mixed
Library removal, real category/service drops, and clipboard import through the
production shell. No QML warnings were recorded by the integrated journey.

The new bounded category viewports exposed a test that dragged cached delegates
outside their visible viewport. The workspace test now scrolls each identified
row into view before dragging. At fractional scale, the import test read the
old content extent before the measured model's layout pass; it now waits for
layout and verifies Add is inside the viewport before clicking. A failed import
prerequisite also stops the test instead of cascading into invalid assumptions.

Reproduce the populated layout/interaction capture run separately at scales 1,
1.5 and 2, substituting the scale and capture prefix:

```sh
QT_SCALE_FACTOR=1.5 QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
OPENSHOW_LIBRARY_CAPTURE=/tmp/openshow-library-final-s1.5 \
./build/tests/library_service_ui_tests
```

All three runs pass. Each writes the import page and populated sidebar at
1000×680, 1380×800 and 1680×1020. The nine sidebar frames are layout evidence;
they do not replace audience-rendering or physical-display acceptance. Outputs
are temporary `/tmp/openshow-library-final-s*-*.png` artifacts, reproducible with
the command above. Full renderer/media and physical M7 suites were not rerun
for these UI/catalog changes. Remaining TV/TA work and FO gates are unchanged.
