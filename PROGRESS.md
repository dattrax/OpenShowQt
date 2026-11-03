# OpenShowQt progress and handoff

Updated: 2026-11-02 (Europe/London).

## Current checkpoint

Current work: **Bible import is DONE — BI.1 through BI.6 are all DONE**
(2026-11-02). BI.1: the provider access approach is recorded in
[docs/ui/bible-import-plan.md](docs/ui/bible-import-plan.md) (passage-only
HTTPS fetch of the ONE requested Bible Gateway page, robots-allowed path,
no corpus, attribution carried; live markup facts recorded the same day),
and the Qt-free `openshow::importer` parses that page into ordered verses —
boundaries from the page's own book-chapter-verse spans, entities and
Unicode preserved, headings/cross-references/nav excluded — refusing
invalid references, blocked pages, changed markup and translation
mismatches without importing anything (`bible_import_tests`, verified
against the live ESV/NIV/error pages). BI.2: the frontend's `BibleImport`
session drives the lookup — one in-flight HTTPS request with a transfer
timeout, explicit no-cache attributes and a 6 MB body cap, a generation
counter so a superseded or cancelled reply answers to nobody, its own
sentences for offline, timeout and HTTP errors, and verses that die the
moment the translation moves; fetched bytes exist only in memory until Add
(`bible_lookup_tests` on a scripted manager). BI.3: the importer's one
destination action landed — `addScripturePassage` writes a **schedule-owned
show** (new domain flag + `ScriptureProvenance` of reference, translation
and licence, stored as optional format-6 keys that ordinary files never
gain) plus its slides and its row on today's schedule as ONE undoable edit
against an explicit target; the Library list, count and drawer search never
see the item, save/reopen keeps the words and provenance, and removal
leaves nothing behind (`domain_tests` + `document_controller_tests`). BI.4:
pagination now measures instead of guessing — protocol minor 14's
`text.measure` command answers "how tall is this block at this width and
size?" from the worker's own SkParagraph/ICU layout (capability
`text.measure.v1`; nothing drawn, stored or woken), the Qt-free
`paginateScripture` packs whole verses and splits long ones at sentence
then word boundaries through the resolved theme's box, and the reference
and attribution reserve measured room rather than riding on the slide's
words (`text_measure_tests` on a live worker + `scripture_pagination_tests`
properties). BI.5: the page landed — More ▸ "Import Bible Passage…" opens
`BibleImportPage`, whose `ScriptureImport` session packs the selection
through the worker's own layout WITHOUT ever blocking (a cache miss sends
its batch and suspends; the reply fills a content-keyed cache and the run
restarts), previews each generated slide through the editor's off-air
selection transport, and offers one door: Add writes today's schedule as
one undoable item with the verse labels in the slides' NOTES and then
CONSUMES the batch, so a repeated click has nothing left to duplicate; no
verb for the Library or for air exists to offer (`bible_import_page_tests`
on a live worker + the BI.5 rows of `workspace_ui_tests`). BI.6: the
milestone closed end to end — `bible_import_e2e_tests` walks the connected
import into a save, then reads the SANDBOXED DISK (HOME and the XDG
directories moved into the scratch tree before the app starts) and finds
no fetched-page bytes and no deselected verse in any file, the kept words
living only inside the service's own records; the provider then goes
unreachable, the next lookup fails on its own actionable line while the
saved row stands, and a fresh controller plays every saved slide on the
real output with zero further network asks — offline playback because the
schedule is the passage's only durable owner. The operator's account of the
whole journey now lives in `docs/architecture.md` under "Online Bible
import". With BI closed, the operator's first live run (2026-11-02) filed
three follow-ups — Add not waking after a live lookup, a reference field
with no book help, and a translation picker that opens unselected —
recorded as milestone **BI-F (BIF.1–BIF.4)** in the PLAN ledger and
[docs/ui/bible-import-plan.md](docs/ui/bible-import-plan.md). **BIF.1 is
DONE (2026-11-02):** the production-shaped fixture named the cause — the
reference and attribution reserved room at the WORDS' display size, so a
theme with no size of its own measured the licence at the renderer's ~96 px
default, ate 720 px of the box, and ground a chapter into 94 one-line slides
behind an asleep Add. The reservation is now caption type (still the
worker's measurement, just set small as provenance is), the chapter
measures into 19 multi-verse slides and one Add lands it, a batch nobody
answers retires after a minute with its own sentence, and no path sits on
"measuring". **BIF.2 is DONE the same day:** the picker's
`indexOfValue` binding — untrackable by the engine, the reason it opened
empty — is replaced by an expression over the session property; opening
the page now shows NIV — New International Version selected, session-side
changes move the picker, and the untouched default sends a byte-identical
request to a menu-chosen one. **BIF.3 is DONE (2026-11-03):** a Qt-free
table of the 66 books and their common abbreviations lives in
`openshow::importer/bookindex`, matched case-, whitespace- and
ordinal-insensitively with exact matches ranking above prefixes ("1 cor",
"1cor", "i cor" and "1st cor" all name 1 Corinthians; "jn" names John) and
a typed chapter tail carried whole into the completion; the reference
field's dropdown filters with every keystroke, Enter or a click finishes
the book, and text matching nothing shows no rows and still looks up
verbatim — the helper never gates and no book list crosses the network.
Two popup truths came out of measuring the live widget rather than
guessing: a Popup sizes itself to its `contentItem`'s implicit size (the
ListView had to become the content item or its rows sat outside the
popup's own 4-px click area), and Material's enter/exit fades defer
`visible=false` until the transition finishes — so the completer nulls
both and retires the instant its completion is taken. **BIF.4 is DONE the
same day:** the end-to-end fixture walks the production shape after its
BI.6 journey — the completer answers "1 cor 1" with the whole chapter, a
fresh session's untouched default is proven (from the captured request
URL) to be exactly the NIV request the menu would have sent, the chapter
adds beside the first passage, the sandboxed disk gives the same
zero-cache witness again, and a disk-only controller plays every chapter
slide with the network counter frozen — the never-substitute rule proving
itself when a fixture body answering the wrong translation parsed to zero
verses. With BI-F closed, the operator's three live-run defects are all
fixed; **TF (theme-driven pagination) is next**, stopping before M7.
**TF.1 is DONE (2026-11-03):** a theme carries named **text contexts** —
typography and box overrides, full stop — and a slide names the one its
words are set in. Format 7 writes them and refuses `contexts`/`textContext`
in a file that predates the concept; inside an entry the vetting is
stricter than anywhere else in the package because an unknown key is
refused naming the rule it breaks — *never a line count or a colour*: the
number of lines a slide holds is whatever the worker's measurement fits,
so the field does not exist to store. The resolver answers customization >
context > themes (the first theme in chain carrying the name supplies it,
wholesale; colours never come from a context), `box` places directly and
`inset` shrinks whichever box the slide would otherwise land on — themed
rectangle or authored primary. Measured, not argued: the reading-size
passage compiles and draws at the context's typography inside the
context's box where the display size wrapped, the song slide of the same
theme draws **byte-identically** before and after the theme grows the
context, and the save/load round-trip draws byte-identically
(`domain_tests`, `scene_render_tests` CPU rasters; gates 63/63 desktop
serial, 45/45 headless).
**TF.2 is DONE the same day:** the packing core left the scripture
paginator and became **`openshow::importer::textflow`** — the generic
measured text-flow engine, which knows blocks (an id and lines), a box, a
typography, a reserved height and the measuring port, and nothing about
verses. Greedy whole-block packing, the sentence-then-word split, the
continuation edges, the `measureReservation` sum and every honest refusal
live there once; `paginateScripture` is now verses-as-blocks plus the
verse-range labeler plus the caption-type reservation, and BI.4's suite
passes it **unchanged** — the extraction moved code, not behaviour.
The design rule is measured as a property: the same four stanzas packed
under a stanza labeler come out 4/2/1 slides for 20/40/80 px of box,
because the engine input carries **no line count to override the
measurement** — a reading theme says "reading box, reading size" and the
worker's answer decides how many lines ride. A block that fits is never
even probed as a fragment; a split carries the block's id through both
halves with `(cont.)` and `(continues)` said by the caller's labeler; the
single-word dead end refuses naming the block (`textflow_tests`; gates
64/64 desktop serial, 46/46 headless).
**TF.3 is DONE the same day:** the Bible page's whole flow — the
measurement style, the off-air preview and the added slides — now walks
the **`scripture` text context**. BIF.1's live-shape chapter on the real
worker, into two services whose manifests differ by nothing but the
context: the plain 96 px look measures out **19 slides**, the theme's
32 px lectern box measures out **4** — seven and eight verses to a slide,
because that is what the worker's measurement fits and nothing else; the
words are exactly the chapter, the preview compiles the same box the
audience will draw into, and every added slide carries the context's
NAME into the file so playback resolves what the preview measured, with
labels in notes and provenance on the show exactly as BI.5 keeps them.
The sibling without the context answers 96 px in the built-in box and
its whole existing suite passes untouched — the name is no layer until a
theme grows it. The fixture also caught a latent **undefined behaviour**
the resolver had been surviving by layout luck: a context-property helper
dereferenced the NULL pointer-to-member the colours (which a context can
never speak) are passed as; guarded now, and the page fixture that found
it keeps the path honest (`bible_import_page_tests`; gates 64/64 desktop
serial, 46/46 headless).
**TF.4 is DONE the same day, closing the milestone:** the Theme Editor's
inspector gained the **Scripture Reading** section — the toggle creates the
`scripture` context out of the theme's current look and box, size,
alignment, spacing and the reading box commit through undoable
controller verbs (an empty field is silence the themes answer, a refused
value snaps every field back, and remove refuses when there is nothing to
remove), and the section states "the box holds ≈ N lines of this size" as
**information only**, measured through the same `text.measure` port the
pagination answers on — no worker, no number, just the honest sentence.
There is still no line-count field anywhere. The resolver itself now
reports which context layered (`ResolvedAppearance.contextName`), so the
Bible page's plan sentence says the passage was "measured to fit **in the
theme's scripture context**" in the resolver's own word, and stays
byte-identical when none answered. The e2e re-acceptance walks the whole
route — context applied through the editor's verbs, the chapter looked up
once, Add, save, reopen: the schedule holds exactly the previewed slides,
every saved scene compiles at the context's size inside its box, and the
presented PGM picture is the previewed flow (`workspace_ui_tests` TF rows,
the TF leg of `bible_import_e2e_tests`; `document_controller_tests` pins
the verbs' merge, refusals, one-undo steps and disk round-trip;
`docs/architecture.md` gained the delivered-shape section and
`docs/ui/text-flow-plan.md` its as-built close; gates 64/64 desktop
serial, 46/46 headless).
The operator's paging note
— flow the whole downloaded passage with the theme deciding how much text
a slide carries (their examples: two display lines for songs, six for a
reading; the line count is what the theme's bible box measures out, never
a fixed split) — is recorded as milestone **TF (TF.1–TF.4)**: named
typography-and-box contexts on
the theme, resolved by the one appearance resolver, and the packing core
extracted from `paginateScripture` into a generic text-flow engine with
scripture as its first adapter; design and as-built in
[docs/ui/text-flow-plan.md](docs/ui/text-flow-plan.md) — **all four
leaves delivered**, as recorded above.
Before BI, **the Themes amendment
(TH.9) is DONE** — the built-in look is now a real, embedded, undeletable **Default** theme (reserved id `default`,
seeded first, re-ensured after every successful load, the chain's last
layer; editable like any theme but refused by delete, with the editor's
Delete button asleep while it is selected), and **every** geometry write on
a slide's PRIMARY lyric box records a placement customization even when no
theme speaks — the authored rectangle is read-only legacy underneath, so a
themed slide nudges as one revertable unit and one restore hands it back.
The Edit page's owner column answers THIS SLIDE / THEME PLACES / AUTHORED
against the APPLIED rectangle, and the captures lead the theme list with
Default. The next planned milestone is **Bible import (BI)** — one requested
passage at a time from Bible Gateway into today's schedule; **M7**
(recovery, budgets, packaging and service acceptance) remains the release
milestone.

Earlier in this amendment, **TH.7 + TH.8 are DONE** — a theme now
carries the PRIMARY lyric box's rectangle, so "the same look" means the same
place too. Placement resolves on the existing chain (slide placement
customization → current theme → slide theme → show default), drives the
**first text item** only, and is absent-by-default so no existing document or
theme moves. The surfaces shipped with it: the Theme Editor's sample overlay
carries the slide editor's own drag/handle gestures, one commit per gesture,
X/Y/W/H fields and a "No placement" clear (the built-in lyric box shown
dashed while the theme has no opinion), and the Edit page routes a placed
box's geometry to a placement CUSTOMIZATION under the THEME PLACES/THIS SLIDE
owner label with a one-unit revert.

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
| Themes TH.8 acceptance (2026-11-01) | `document_controller_tests` placement surfaces — applied-rect rows and `boxAt` hit test, geometry writes routed to a placement customization while the authored rect stays untouched underneath (proved by parsing the saved file's own JSON), per-field refusal on an unplaced box, revert-one-unit, and the sample/createThemeFromCurrent/duplicate carry the placement; `workspace_ui_tests` drives the shipped UI — the editor's fields asleep on a theme with no opinion, dragging the dashed OFFER makes a placement moved by exactly the pointer's travel, one undo removes the whole gesture, four typed numbers write one unit, a refused width changes nothing and the fields snap back, No placement clears the rectangle and leaves the theme intact, and a page re-shown after outside edits still reads the record (the stale-binding bug this check caught ships with the fix); on the Edit page THEME PLACES names the owner, a typed customization flips it to THIS SLIDE with the revert live, the revert hands the whole rectangle back, and a new box is unplaced and keeps authored edits; captures inspected — the Edit page draws the canvas chrome at House's 240/620/1440/320 under THEME PLACES and the Themes page shows that rectangle solid with the fields holding its numbers; 57/57 desktop and 43/43 headless; evidence in [the plan](docs/ui/themes-plan.md) and [the architecture](docs/architecture.md) |
| Themes TH.9 acceptance (2026-11-02) | `domain_tests` — a fresh service is seeded with exactly the Default theme, its look checked property-for-property against the built-in it replaced, unthemed slides resolve through the floor, Default renames and places like any theme but removal is refused naming itself, and a pre-amendment manifest opens healed with Default re-ensured; `document_controller_tests` — a drag on the UNPLACED primary box of a slide nothing places lands in `customizations.placement` with the saved file's authored rectangle unchanged (proved by parsing the manifest's own JSON), rows carry the applied rectangle and the `primary`/`placed` flags, and one revert hands the slide back to its authored box; `workspace_ui_tests` — More opens on the Default row with the editor still offered, Delete sleeps while Default is selected, and with only Default left every targeted control disables while a directly-asked removal is refused; captures inspected with Default leading the list and the THEME PLACES story unchanged; 57/57 desktop and 43/43 headless |
| Bible import BI.1 acceptance (2026-11-02) | `bible_import_tests` — live-shape passage page parses to ordered verses (chapter/verse boundaries, `&ldquo;`/numeric entities, literal Unicode), headings/cross-reference anchors/verse numbers/site furniture never ride in a verse, the attribution block arrives as licence text; refusals: no-results, blocked, changed-markup, empty page, and a translation that answered something else (names both, imports nothing); translation menu carries NIV and KJV; the same parser verified against the live ESV, NIV and invalid-reference pages on 2026-11-02; 58/58 desktop and 44/44 headless |
| Bible import BI.2 acceptance (2026-11-02) | `bible_lookup_tests` — scripted `QNetworkAccessManager` with fake replies: opens on the internet notice; the request is HTTPS with encoded `search`/`version`, a transfer timeout, `CacheSaveControlAttribute=false` and `AlwaysNetwork` load (nothing fetched is ever cached); success pre-selects verses and carries the attribution; offline says the notice and retry re-asks; the timeout has its own sentence; cancel retires the reply so it cannot write a passage later; a superseded reply leaves the newer passage standing; the 6 MB cap refuses unparsed; HTTP 403 answers with its number; an empty reference sends nothing; changing the translation clears the stale verses and attribution; 59/59 desktop and 44/44 headless |
| Bible import BI.3 acceptance (2026-11-02) | `domain_tests` — `createScriptureShow` names itself after the reference, carries ownership and provenance from creation and refuses a blank reference or translation naming each; the manifest gains `scheduleOwned`/`scripture` only on the importer's entry (the Library show's entry gains no keys), the item round-trips field-for-field through `sameService`, a format-1 file claiming either key is refused naming it, and provenance on a show that is not schedule-owned is refused; `document_controller_tests` — one `addScripturePassage` lands show + two slides + the schedule's first row, the Library list/count and the drawer search stay clean of it, one undo removes the whole item and one redo restores the same identity, the saved manifest carries the provenance, a reopened controller reads the row back with the selected words while the Library stays empty, removing the schedule row leaves nothing, and an Add against a schedule that does not exist is refused naming the target; 59/59 desktop and 44/44 headless |
| Bible import BI.4 acceptance (2026-11-02) | `protocol_tests` — hello serves minor 14 and names `text.measure.v1` (both capability tripwires moved to 17); `text_measure_tests` on a live `openshow-worker` — the same line at size 80 measures taller than at 40 with the same line count, the same text wraps to more lines in a narrower box (real SkParagraph/ICU layout answering, not a character count), Unicode words measure and wrap, an empty block measures zero, a named family measures and an unknown one falls back rather than refusing, a batch answers whole and in order with keys echoed, and every cap refusal (over-batch naming the cap, spacing outside the layout's band, zero width, missing key/lines, over-cap lines) names itself while the session lives on; `scripture_pagination_tests` on a scripted monotonic engine — whole verses pack one per line, a narrow box or a larger chosen size means MORE slides never smaller words (every ask carries the chosen size), long verses split at a sentence boundary (`.`/`!`/`?`/danda followed by whitespace, ellipsis-safe) falling back to word boundaries, every continuation slide still names its verse, concatenated slide text equals the selected passage after whitespace stripping in every case, no split ever lands inside a UTF-8 character, the reference/attribution reserve exactly their measured height and an attribution that eats the box blocks Add with "leave no room", and a zero-width box, a NaN height, a single unfitting word (naming its verse), a dead engine (latched, its own sentence passed through) or a shuffled/short answer each refuse with nothing generated; 61/61 desktop and 45/45 headless |
| Bible import BI.5 acceptance (2026-11-02) | `bible_import_page_tests` on a live `openshow-worker` with a scripted network — the page opens on the online notice with nothing addable and Add refusing rather than inventing a passage; a lookup answers and the selection paginates on its own, the slides' words exactly the selected passage and `wordsOnAir` false through generation and a clicked preview; wearing a small-type theme re-paginates with NO second network request — more slides with a `(cont.)`/`(continues)` split of the long verse, identity intact, and the wide box converges back; Add lands exactly one schedule row and nothing in the Library, the consumed batch makes a repeated Add refuse, one undo removes the whole item and one redo restores it, and the saved service.json carries the labels in the slides' notes with no label text on any slide's words; cancel discards words and slides, says nothing was written, and moves no schedule row; without a worker the run settles with no slides and a sentence naming the missing renderer (never estimated slides), and an unreachable provider leaves the page on the lookup's own actionable line; `workspace_ui_tests` — the More menu carries the entry, the page opens off screen on the notice with Add asleep and naming today's schedule as its only destination, and reading the page starts no output and writes no schedule row; 62/62 desktop and 45/45 headless |
| Bible import BI.6 acceptance (2026-11-02) | `bible_import_e2e_tests` on a live `openshow-worker` with HOME, XDG_CONFIG_HOME, XDG_CACHE_HOME and XDG_DATA_HOME sandboxed into the scratch tree before the application exists — the connected (scripted-provider) lookup, deselection, Add and save finish with exactly one network ask; the whole sandbox is then walked file by file: the fetched HTML's own furniture (`passage-text`, `publisher-info-bottom`, version classes) appears in NO file, the deselected verse's words appear nowhere, and the kept words only under the service's own directory; the provider is then refused (`HostNotFoundError`) and the next lookup fails on its own line with the saved schedule row unchanged and nothing addable; a fresh `DocumentController` reopens the service from disk and every saved slide plays through the ordinary one-call `presentSceneJson` go on the real output (`wordsOnAir` live, two slides at least) with the network count unchanged at two — saved scripture plays with the provider down; fixtures stay synthetic (no live provider in the suite; the parser's live checks are recorded in the plan); the operator account is `docs/architecture.md` "Online Bible import"; 63/63 desktop and 45/45 headless |
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
key are refused. **Themes amendment TH.8 DONE 2026-11-01** closes it: the
Theme Editor's sample overlay carries the slide editor's own gestures (drag,
corner handles, one commit per gesture, X/Y/W/H fields, "No placement"), the
Edit page routes a placed box's geometry to a placement CUSTOMIZATION with
the THEME PLACES/THIS SLIDE owner label and a one-unit revert, and the
capture pair shows one rectangle on both pages.

**Themes amendment TH.9 DONE 2026-11-02**, one pushed commit: the built-in
look became the embedded Default theme — seeded first, re-ensured after every
load, the chain's last layer, edited freely but never deleted — and every
write to a slide's primary lyric box now records a placement customization,
the authored rectangle read-only legacy beneath the chain. Operator note: a
slide whose authored primary rectangle was dragged before the amendment
(the legacy slide 1's y=261) keeps resolving to that authored rect until the
four numbers are typed once; pressing Save then migrates the durable file.
**Bible import (BI)** — online passage lookup into today's schedule — is the
next planned milestone in [PLAN.md](PLAN.md); **M7** remains the release gate.
