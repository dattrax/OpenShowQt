# OpenShowQt progress and handoff

Updated: 2026-11-04 (Europe/London).

## Current checkpoint

**TS.4 complete (2026-11-04): the operator picks fonts the AUDIENCE's renderer
actually has, and every typography control edits exactly one field.**
`font.list` (protocol minor **17**, capability `font.list.v1`, hello now 20)
is answered by `SceneTextEngine::listFonts()` — the same font manager the
frame loop shapes with, which is the only honest answerer because that
renderer falls back silently from names it cannot match. Metadata only,
never font bytes.

The wire is bounded and the list is one scan: params are nothing or
`{from}` (any other key, and a negative/non-integer `from`, refused by
name); a page is capped at `kMaxFontListPageFamilies` **128** so one reply
stays far inside the 1 MiB frame however many long PostScript names
fontconfig knows, and answers `{families, from, total, truncated}`. The
scan happens ONCE per worker lifetime under its own mutex, on the RPC
dispatch thread — never a presentation or audio callback — and every later
ask copies the cache. A `from` past the end is an EMPTY page, not an error:
a list shrinking under a mid-walk asker is legal, and re-reading from 0 is
the asker's repair, not the protocol's. A session with no engine returns
its own error sentence instead of zero families, because "this box has no
fonts" is not what happened.

`WorkerController` owns the walk per CONNECTION
(`fontListSupported`/`fontListState` idle→loading→ready, `unsupported` for a
hello without the flag, `unavailable` for a failed ask/`fontFamilies`/
`fontFaces` + `refreshFontFamilies()`), and retirement is keyed by REQUEST ID
with a continuity check on the echoed `from` — a page from a dead session or
a superseded walk is dropped silently, so two sessions can never splice one
list.

The inspector writes through a new one-field merge verb,
`setThemeTextStyleField(themeId, field, value)`, beside TS.1's whole-set
verb: 13 leaf fields plus `"stroke"`/`"shadow"` for the inherit buttons;
null clears a scalar to inherit or names a whole-record clear and is REFUSED
on an effect member (an effect means one thing whole); merging a member into
a theme with no such record materialises the contract's defaults — exactly
the pixels the audience draws today — so enabling an outline gives
`{true, #000000, 2}`, never a fragment or another theme's leftover; exactly
one `edit()` per call, and the merged block passes the domain's one
`validThemeTextStyle` sentence, so a refusal changes nothing and costs no
undo. Controls show the theme's own value where it carries one and the
RESOLVED answer otherwise, read through `themeSampleAppearance`'s new
resolved-style extension, with a badge naming which. Because TV.3 has not
promoted the reading context to a view of its own, these route to the base
theme (Songs); the scripture context carries no style fields until then.

Gesture discipline is the tested half, not an intention: sliders commit ON
RELEASE (`onPressedChanged`, Qt 6.12 `Slider` has **no** `onReleased`) and
write nothing while held; a field committed unchanged writes nothing AND
costs no undo; a refused write re-reads the record and snaps the control
back; the typed family works with no worker at all; the note refuses to
invent a verdict ("this worker cannot list its fonts" / "checking…" / "not
installed on the audience's machine — it will fall back"), MISSING is judged
only from a COMPLETED worker list, and the stored name is NEVER rewritten;
Browse sleeps until a listing worker is ready, so no click opens an empty
picker.

The tests also found a real bug: the rail's scroll verbs asked
`inspectorContent.parent` for `contentY` — that is the Flickable's own
content item, which cannot scroll — so scrolling did nothing at all. They
now walk parents for a node carrying BOTH `contentY` and `contentHeight`,
compute the end themselves (a Flickable does NOT clamp a handed `contentY`;
past the last row is blank paper), and gained `scrollInspectorTo(name)` —
reachability is a promise about every control, not only about the ends.

Evidence shape: `text_measure_tests` pins the real fontconfig answer (sorted
page, ascending faces, `truncated` exactly at the cap, a repeated ask
byte-identical to the first = one cached scan, page 2 continuing name for
name, from-past-end empty, by-name refusals with the session living on);
`worker_controller_tests::scenarioFontListWalk` choreographs a blind peer
(`unsupported`, ZERO `font.list` frames on the socket), a two-per-reply fake
paging a five-family scan through `from` 0,2,4, disconnect mid-walk retiring
the state and dropping late pages, a superseded refresh retiring the older
walk by request id, and an error reply landing `unavailable` with the
worker's sentence; `document_controller_tests` covers every field band and
refusal, defaults materialising around one member, whole-record clears, one
undo per gesture, the disk round-trip, and preview-vs-saved agreement (the
sample scene's first text item carries the same `stroke`/`fontWeight` as the
compiled audience slide); `workspace_ui_tests` drives the real controls
(combo writes its own field while neighbours keep silence, undo repaints the
control from the record, unchanged commits cost no undo, an off-band minimum
size snaps back, the outline toggle materialises `{true, #000000, 2}`,
un-ticking keeps `enabled: false`, "Off" clears and sleeps, a held blur
handle writes nothing while held / commits once on release / one undo backs
the whole drag, a typed family saves with the "cannot list" note while Browse
sleeps and opens nothing), with the geometry loop now checking the true last
control `themeEditorShadowBlur` at all three window sizes and the keyboard
leg scrolling by name.

Gates: desktop `ctest -j1` **64/64** (~381 s), headless build + `ctest -j1`
**35/35**, `git diff --check` clean.

Earlier this plan: TS.3 (`c2e6645`) styled measurement end to end (minor 16,
`text.measure.v2`, `ink`/`fillInk`); TS.2 (`e4258e9`) the styled renderer and
the advertised `scene.text-style.v1`; TS.1 (`c5acad7`) the style contract
(service format 8, scene format 4, minor 15); TV.2 (`7331a10`) the Themes
layout; TV.1 (`0f1cf84`) the sample-based inspector previews.

## Where the work stands

Phases 0–5 complete. Phase 6 complete through 6.26i. The Bible plan
(BA.1–BA.8) is DONE. The themes plan: TV.1, TV.2, TS.1, TS.2, TS.3, TS.4
DONE. Next is **TS.5** — reproducible font/effect raster and performance
acceptance: pin the review's Inter/Lora/Oswald revision with TTF hashes and
each `OFL.txt` in an isolated OFFLINE fixture setup, then test fallback and
Unicode coverage, the weight ramp, effect edges and negative offsets and
midpoint fades, with missing fixtures failing (or skipping) VISIBLY and no
unbounded preparation/render cost. Then TR.1–TR.3 (transitions; **TR.1 claims
service format 9**), then the TV.3/TV.4/TV.5/TA.1 leaves this run excluded.

## How to pick up

1. `PLAN.md` is the contract: ledger rows carry acceptance evidence, the
   themes runbook paragraphs give each leaf's boundary, and "Format
   allocation" records the current **8 / 4 / 0.17** (TS.4 took the minor for
   `font.list.v1`; it adds no persisted meaning, so service and scene stay
   where TS.1 left them). `docs/reviews/theme-editor-2026-11-03.md` is the
   design authority.
2. Build `cmake --build --preset desktop --parallel 2` (build dir `build/`);
   test `ctest --preset desktop -j1` (~6.5 min). Headless: `--preset
   headless` (`build-headless/`, 35 tests). Individual binaries:
   `./build/tests/<name>`; `text_measure_tests`, `worker_controller_tests`
   and `live_fade_tests` take the worker binary as argv[1]
   (`./build/src/worker/openshow-worker`). ctest names are target-ish, not
   file-ish: `rpc_ipc_tests` runs as `worker_rpc_lifecycle`. Rebuild the
   worker binary after renderer/protocol changes — a stale one once faked a
   green suite.
3. TS.5 starts in the test fixtures, not the product: a pinned, offline,
   checksummed font set that the raster suites load explicitly (the CI
   machine's fontconfig set is not a reproducible input — today's fixtures
   lean on DejaVu). The renderer surfaces it must exercise are all shipped:
   `TextStyle::setFontStyle` for weight/slant faces, `addShadow`, the
   `GroupAlphaPainter` outline-then-fill pass, and the fade multipliers
   TS.2 measured. TS.4's `listFonts()` is a useful cross-check that a pinned
   family is really resolvable, but the acceptance is about PIXELS and cost,
   not about the list.
4. UI test lore, learned the hard way this leaf:
   `QMetaObject::invokeMethod(item, "toggle")` flips `checked` but does NOT
   fire `onToggled` — drive checkboxes with a real `clickAt`. A closed
   `Popup` is not a `QQuickItem` and not in the item tree at all: find it
   with `window->findChild<QObject*>("name")`. `console.warn`/`log` from QML
   is invisible here (warnings are captured and filtered) — diagnose by
   setting a QML property and reading it from C++. QML changes only reach
   the binary through a FULL `cmake --build --preset desktop`; `--target
   <test>` alone does not rebuild the QML resource. Flickable `contentY`
   assignments are not clamped. In `workspace_ui_tests`, `docs.status()` is
   overwritten by "autosave refused after edit: asset 'ghost.mp3'…" planted
   earlier — never assert a refusal through `status()` there.
5. Measurement lore, still true: the blur sigma is device-space (the painter
   rescales), stroke width is CTM-scaled by Skia, the fork's shadow pass
   fires `drawTextShadow` once per run BEFORE the blobs; `LineMetrics`
   fields are `fLeft/fWidth/fBaseline/fAscent/fDescent`; Skia's blur support
   is ~3.09σ (the code claims 3.1, fixture-bounded both sides); SkParagraph
   CHAR-breaks words wider than the box, so the width leg of `fits` is
   unreachable and the height leg is where verdicts arise; fixture renders
   are 960×540 = the authored canvas at 0.5, so containment checks map ink
   through that scale with 2 px slack.
6. Boost 1.90 spells doubles in exponent form; manifest assertions match
   style keys by NAME. JSON fixtures use decimal integers. Never hold a
   `const Theme*` across a call that can grow the theme vector. Qt6 has no
   `QMetaType::QMap`. A ctest "Passed" after a failed build means a stale
   binary — rebuild first, and grep piped builds for `error:|Error [0-9]`;
   multi-main UI tests print nothing on pass.

## Recent commits

- TS.4 — worker-owned `font.list` (minor 17, `font.list.v1`, capped paging,
  request-id walk retirement), the one-field `setThemeTextStyleField` merge
  verb, the inspector's typography/effect controls and font picker with
  commit-on-release gestures and inherit/revert, and the fixed
  inspector-rail scroll helpers.
- TS.3 (`c2e6645`) — styled measurement end to end: `ink`/`fillInk` split,
  `fits`+`bounds` replies, `fitMode:"fixed"`, minor 16 + `text.measure.v2`
  gate, shared full-style cache keys, flow verdict criterion.
- TS.2 (`e4258e9`) — faces/stroke/shadow through public Skia, fade-aware
  painter, measured device-space blur rescale, `scene.text-style.v1`
  advertised, raster + GPU-live fixtures.
- TS.1 (`c5acad7`) — shared text-style contract, service 8, scene 4,
  protocol minor 15, capability gate, controller surfaces.
- TV.2 (`7331a10`) — full-workspace Themes layout, view thumbnails.
- TV.1 (`0f1cf84`) — theme sample views through the preview transport.
