# Song titles, opening cues and section captions — 2026-11-06

The operator requested title-only clipboard metadata, a blank opening cue and
Verse/Chorus/Bridge captions with colours that fit the charcoal/blue interface.
Consecutive slides in the same section should show the heading once.

`a8101d0` handles the title and blank opening. A single non-label line before
the first explicit section (with optional blank lines) supplies the editable
title and is excluded from lyrics. Unlabelled input and multi-line preambles
keep all words. A title with no subsequent words refuses import. CCLI/author/
copyright handling stays in the existing metadata path.

`SongPlanner` prepends one wordless cue labelled Blank to its successful plan.
The import page and service bridge use that same plan. It is not an empty
stanza in the catalog. The service accepts the explicit blank marker only on
a wordless first slide followed by lyric slides; invalid plans leave the
service unchanged. Saving/reopening retains the blank and prepared words.

Existing stored `Slide::group` labels become preview-row `sectionLabel` values.
The shared card normalises display spelling and supplies the palette:

| Section | Caption colour |
|---|---|
| Verse | Muted blue `#28495f` |
| Chorus / Refrain | Teal `#25534e` |
| Bridge | Purple `#514363` |
| Pre-Chorus | Indigo `#3b4d71` |
| Post-Chorus | Green `#375542` |
| Blank / Intro | Slate `#454954` |
| Ending / Outro | Amber `#62543a` |
| Instrumental / Interlude | Ochre `#5a503e` |
| Tag / Vamp | Mauve `#604351` |

Show, Edit and clipboard preview compute boundaries from complete source order,
so a scrolled-off predecessor still suppresses a continuation's heading. Case
changes such as Chorus/CHORUS are the same section. A returning chorus after a
bridge receives its heading again; service occurrences reset the boundary.
Continuation cards retain a coloured strip and full lyric picture. Show retains
the service item's name on its opening card. Live red and selection outlines
retain their established meaning. Ordinary Bible/presentation captions keep
their previous shape when no section metadata is present.

Saved songs can be reimported to regenerate their title/blank opening. Caption
styling reads existing section metadata directly; this change does not rewrite
existing catalog records or service slides during startup.

Focused checks cover title recognition and preservation, title-only refusal,
explicit-blank plan validation and reopening, real worker import/service plans,
canonical captions, palette distinction, live priority, continuation suppression
in all three hosts, offscreen predecessors, returning sections, actual Thumbnail
and Selection frames, and no accidental presents or QML warnings.

Set `OPENSHOW_LIBRARY_CAPTURE=/tmp/openshow-song-final` when running
`library_service_ui_tests` to save the populated journey plus
`-sections-show.png` and `-sections-edit.png` with real lyric pictures.

Validation: the desktop build succeeded, and all 19 affected suites passed
(66.61 s): lyrics, song pagination, song import, song service, import page,
Library/service UI, Library removal, workspace UI, shared controls, UI loading,
document controller, inspector adapter, domain, catalog, scene, catalog
controller, workspace panels, Library search and PowerPoint UI. The
Library/service journey also passed at `QT_SCALE_FACTOR=1.5` (13.36 s).
Populated Show/Edit captures at both scales were visually reviewed, including
actual worker-rendered lyrics on continuation slides. `git diff --check` passed.
