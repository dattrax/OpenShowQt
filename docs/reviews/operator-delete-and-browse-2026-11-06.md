# Delete and service-browsing fixes — 2026-11-06

This follow-up supersedes the earlier LRF.3/LRF.4 Remove-button and reversible
Library-delete behavior. The operator asked for keyboard removal, Shift+arrow
service selection, a permanent database-deletion warning and playable service
rows after clicking them.

## Behavior and rollback checkpoints

- `24458a4`: Show resolves the actual service show through `showInfo`, rather
  than searching `docs.songs`, which intentionally excludes prepared songs,
  scripture imports and retained service-only presentations. Slides, title and
  show-default theme all use the actual show identity.
- `b08817f`: Shift+Up/Down extends/contracts the service range around its
  original anchor, scrolls to the current entry and stops at either boundary.
  Delete removes selected entry identities in one undoable edit, preserving
  other occurrences and Library sources. The service header Remove button and
  Edit-mode row deletion buttons are removed; reorder arrows remain.
- LRF.7: The Library header Remove button is removed. Focused Delete and the
  context-menu deletion action open the same modal warning. Cancel receives
  focus; Enter, Escape and Cancel cancel without changing records or selection.
  The explicit Delete permanently action applies the captured selection only
  after validating every identity again. Text-field Delete remains native.

Confirmed Library deletion removes those identities from older undo/redo
snapshots, retaining unrelated history. Referenced presentations remain as
service-only shows; catalog songs detach and retain their prepared slides and
attribution. Catalog links are also removed from service-history snapshots, so
an old undo cannot relink a missing database record. Unreferenced presentations
are discarded. No format migration is involved. These guarantees apply to
confirmed operator deletion; the existing reversible controller operations
remain available to internal workflows.

Catalog batches still write durably before publishing deletion or changing
history. Write failure leaves every record, selection and history intact. A
stale dialog refuses the whole selection. Deleting a song also cancels an
in-flight service add/refresh, retiring late measurement replies before they
can recreate a deleted link.

## Validation

The desktop build succeeds and all **15 affected suites pass** (58.72 s).
`git diff --check` is clean. The build includes the actual `build/src/ui/openshow-ui` executable.
`library_removal_tests` walks every older undo/redo snapshot after permanent
catalog and presentation removal, checks stale IDs, verifies later service
removal is still undoable, and reopens the retained service copies/database.

`library_service_ui_tests` drives the production shell with a real worker:
populated authored/song/scripture/detached service clicks, compilable slides,
Shift+arrow ranges and scrolling, exact duplicate-entry deletion, one-step
service undo, modal cancellation, confirmation, failed writes, stale-dialog
preflight, native search Delete, filtered/mixed database deletion and old
service-history traversal. It requires zero unexpected QML warnings and zero
presents throughout. `song_service_tests` additionally deletes songs during
both asynchronous add and refresh and checks that late replies remain retired.

Acceptance command (real rendering worker requires desktop access):

```sh
ctest --test-dir build --output-on-failure -R '^(domain_tests|catalog_tests|scene_tests|workspace_ui_tests|control_set_ui_tests|ui_loads|document_controller_tests|workspace_panels_tests|library_search_tests|library_removal_tests|catalog_controller_tests|song_import_tests|library_service_ui_tests|song_import_page_tests|song_service_tests)$'
```

The optional `OPENSHOW_LIBRARY_CAPTURE=/tmp/openshow-bugfix-final` environment
variable saves populated views at 1000×680, 1380×800 and 1680×1020, plus the import
page and the permanent-deletion dialog. These are layout/interaction evidence;
physical audience-output release gates remain separate.

The final capture run passed. The compact workspace and confirmation dialog
were visually checked: buttons are reachable, the warning is readable, and
removed toolbar actions are absent.
