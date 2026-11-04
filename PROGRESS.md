# OpenShowQt progress and handoff

Updated: 2026-11-04 (Europe/London).

## Current checkpoint

**TS.5 complete (2026-11-04): the typography fixtures now render faces somebody
pinned, and a machine without them says so out loud.**
`font_fixture_tests` draws Inter / Lora / Oswald at the exact google/fonts
revision and SHA-256 the theme-editor review recorded — and because the manifest
is a *copy* of that decision, the suite takes the review itself as argv[2] and
asserts the two agree, before any skip decision, so a quietly drifted pin fails
even on a machine that has no corpus at all — plus `Noto Sans` as the fixed
fallback face the review told TS.5 to add, and each family's own `OFL.txt` (the
four copyright lines differ, so one licence does not cover the set).

The fonts are **not vendored** — 3.3 MB of third-party binaries for a handful of
fixtures is a packaging-policy question, not a test decision. `tools/
setup-font-fixtures.sh` prepares a developer cache BY HAND (`prepare --download`
or `--from DIR`, which finds either the flat or the upstream layout, plus
`verify`/`status`), hash-checks every file it places and deletes one that
disagrees, and installs nothing into the system. `$OPENSHOW_FONT_FIXTURE_DIR`
else `${XDG_CACHE_HOME:-$HOME/.cache}/openshow/font-fixtures` is the one
directory script and test share. **ctest never downloads.**

Three corpus states, three honest outcomes: the suite re-hashes all eight files
itself (the sha256 is written in the test, which is Qt-free, and self-tested
against published vectors including the 56-byte length where padding spills a
block) — **absent** is exit 77, which ctest prints as `Skipped` with the setup
command in its output, while **corrupt** is a FAILURE naming the file and both
hashes. "You have not prepared the corpus" and "your corpus is not the corpus we
pinned" are the difference between an honest skip and measuring a different font.

Isolation is real: a private `fonts.conf` naming ONLY the corpus directory, with
its own `cachedir` and **no `/etc/fonts` include**, is set before the engine
exists, `XDG_CACHE_HOME` moves too — and `fixtureCorpusIsolation` then asserts
the engine's OWN `listFonts()` scan names exactly the four pinned families while
`OpenShow No Such Family 47` resolves to nothing. Resolution before pixels,
because every claim downstream is about a named face.

What is now proven on those faces: body/caption line heights hold the 64/28
ratio within 5%; four files really are four faces; weight 100→900 resolves a
face (>20% more ink) **without moving or re-wrapping a line**; the widest legal
stroke grows each edge by half its pen and a sigma-64 shadow at −40/−30 reaches
20 px left and 15 px above the fill's box, with every pixel of both effects
together inside TS.3's `ink` at the 0.5 snapshot scale and `fillInk` **exactly
unmoved** — an outline must never be able to shift a `fits` verdict; shrink fits
its box while `fixed` keeps the size and visibly overflows it; the deliberate
missing family falls back to a face that is IN the corpus, byte-identical to
rendering that family by name; and the per-family glyph matrix is printed and
claimed only where the face has the glyph — no pinned face covers Hebrew, CJK
or emoji, so those arrive as the placeholder box, which is the corpus's
boundary stated rather than a product promise.

Two measurement lessons are in the code, both found by the suite disagreeing
with reality: an absolute background threshold reads a half-bright antialiased
edge as a word that shrank by 2 px, so the fade's placement is compared at each
frame's OWN peak (the boxes are then identical); and the three passes are
separated by HUE rather than distance-to-authored-colour, because a blurred
shadow blended with the backdrop has no pixel near `#00FFFF` at all. With those
rulers, fill/stroke/shadow attenuate to median ratios 0.581 / 0.492 / 0.502.

Costs are the record, and printed cold by ordering (this fixture runs before
anything touches the engine): whole-corpus scan 0.4 ms then 0.00 ms cached,
first shape ≤0.9 ms per family and ≤0.01 ms steady, 12 scene builds in 1.7 ms
with the layout cache at exactly four resident / fifth evicted / fifty draws
hitting ONE retained layout — finite occupancy after churn and no re-shaping on
a steady draw, observed by pointer identity rather than inferred from a
stopwatch. **No product change was needed**: TS.1–TS.4 already ship every
surface these fixtures exercise, and TS.5 claimed no format bump — nothing here
is stored or put on the wire.

Evidence: `font_fixture_tests` (new target, `SKIP_RETURN_CODE 77`, the manifest
as argv[1] and the review document as argv[2]); `docs/fixtures/fonts.md` carries
the record, the skip report and the rulers; `tests/fixtures/fonts/README.md`
carries the licence and the not-vendored policy note. Gates: desktop
`ctest -j1` **65/65** (~383 s), headless build + `ctest -j1` **36/36**,
`git diff --check` clean.

Earlier this plan: TS.4 (`32bcdb7`) worker-owned `font.list` and one-field
style controls; TS.3 (`c2e6645`) styled measurement (`ink`/`fillInk`,
`text.measure.v2`); TS.2 (`e4258e9`) the styled renderer; TS.1 (`c5acad7`) the
style contract (service 8, scene 4, minor 15); TV.2 (`7331a10`) the Themes
layout; TV.1 (`0f1cf84`) sample-based previews.

## Where the work stands

Phases 0–5 complete. Phase 6 complete through 6.26i. The Bible plan (BA.1–BA.8)
is DONE. The themes plan: TV.1, TV.2, TS.1, TS.2, TS.3, TS.4, TS.5 DONE — the
TS group is closed. Next is **TR.1–TR.3** (configurable slide transitions;
**TR.1 claims service format 9** and captures transition options at Go rather
than at prepare ack). The TV.3/TV.4/TV.5/TA.1 leaves are out of this run by
request.

## How to pick up

1. `PLAN.md` is the contract: ledger rows carry acceptance evidence, the themes
   runbook paragraphs give each leaf's boundary, and "Format allocation" records
   the current **8 / 4 / 0.17** — TR.1 takes the service bump to 9.
   `docs/reviews/theme-editor-2026-11-03.md` is the design authority (its
   Transition contract section is TR's).
2. Build `cmake --build --preset desktop --parallel 2` (build dir `build/`);
   test `ctest --preset desktop -j1` (~6.5 min). Headless: `--preset headless`
   (`build-headless/`, 36 tests). Individual binaries: `./build/tests/<name>`;
   `text_measure_tests`, `worker_controller_tests` and `live_fade_tests` take
   the worker binary as argv[1]. ctest names are target-ish, not file-ish:
   `rpc_ipc_tests` runs as `worker_rpc_lifecycle`. Rebuild the worker binary
   after renderer/protocol changes — a stale one once faked a green suite.
3. **To run the font fixtures**: `tools/setup-font-fixtures.sh prepare
   --download` once (or `--from DIR` offline), then
   `ctest --preset desktop -R font_fixture_tests`. Without it the test reports
   `Skipped`, which is the designed outcome, not a failure — and if you change
   the corpus, `verify` before wondering why pixels moved.
4. TR.1 starts in the Qt-free domain: a shared `TransitionOptions` with ONE
   validator, undoable service-default and optional Theme/TextContext setters,
   storage migration (format 9, plus the pre-9-era refusal the allocation rule
   requires), duplication, and `resolveTransitionForSlide(showId, slideId)`
   following the view-inheritance rule the review's transition contract states.
   The options belong to the ORIGINAL Go — delayed prepare and settings changes
   must not re-read them — so the resolver is captured at Go, and TR.2/TR.3 put
   it on the wire and in the UI.
5. Fixture lore, learned the hard way this leaf: `curl` needs `--globoff` for
   these filenames (the `[opsz,wght]` brackets are otherwise a range glob);
   Boost 1.90 spells doubles in exponent form and JSON fixtures use decimal
   integers; `-fno-rtti` is required on any TU that includes Skia headers (the
   CMake block sets it per source); scene JSON colours are packed integers, not
   `#rrggbb` strings; the snapshot CLI is
   `build/src/worker/openshow-worker-dev --snapshot out.png --scene-file s.json`
   at 1920×1080 for the 960×540 authored scene (0.5 device scale).
6. UI test lore, still true: `QMetaObject::invokeMethod(item, "toggle")` flips
   `checked` but does NOT fire `onToggled` — drive checkboxes with a real
   `clickAt`. A closed `Popup` is not a `QQuickItem` and not in the item tree:
   find it with `window->findChild<QObject*>("name")`. `console.warn`/`log` from
   QML is invisible here (warnings are captured and filtered) — diagnose by
   setting a QML property and reading it from C++. QML changes only reach the
   binary through a FULL `cmake --build --preset desktop`. Flickable `contentY`
   assignments are not clamped. In `workspace_ui_tests`, `docs.status()` is
   overwritten by "autosave refused after edit: asset 'ghost.mp3'…" planted
   earlier — never assert a refusal through `status()` there.
7. Measurement lore: the blur sigma is device-space (the painter rescales),
   stroke width is CTM-scaled by Skia, the fork's shadow pass fires
   `drawTextShadow` once per run BEFORE the blobs; Skia's blur support is
   ~3.09σ (the code claims 3.1, fixture-bounded both sides); SkParagraph
   CHAR-breaks words wider than the box, so the width leg of `fits` is
   unreachable and the height leg is where verdicts arise; fixture renders are
   960×540 = the authored canvas at 0.5, so containment checks map ink through
   that scale with 2 px slack.
8. Never hold a `const Theme*` across a call that can grow the theme vector.
   Qt6 has no `QMetaType::QMap`. A ctest "Passed" after a failed build means a
   stale binary — rebuild first, and grep piped builds for `error:|Error [0-9]`;
   multi-main UI tests print nothing on pass.

## Recent commits

- TS.5 — pinned offline font corpus (manifest + setup script + hash checks),
  `font_fixture_tests` over an isolated fontconfig configuration (family
  resolution before pixels, four-face distinctness, weight ramp, body/caption
  metrics, extreme legal effects against the ink bounds, fixed vs shrink,
  missing-family fallback inside the corpus, coverage matrix, midpoint fade per
  pass, cached scan / cold-and-steady shaping / bounded layout-cache records),
  visible skip vs corrupt-corpus failure, and `docs/fixtures/fonts.md`.
- TS.4 (`32bcdb7`) — worker-owned `font.list` (minor 17, `font.list.v1`,
  capped paging, request-id walk retirement), the one-field
  `setThemeTextStyleField` merge verb, inspector typography/effect controls and
  font picker with commit-on-release gestures, and the fixed inspector-rail
  scroll helpers.
- TS.3 (`c2e6645`) — styled measurement end to end: `ink`/`fillInk` split,
  `fits`+`bounds` replies, `fitMode:"fixed"`, minor 16 + `text.measure.v2`
  gate, shared full-style cache keys, flow verdict criterion.
- TS.2 (`e4258e9`) — faces/stroke/shadow through public Skia, fade-aware
  painter, measured device-space blur rescale, `scene.text-style.v1`.
- TS.1 (`c5acad7`) — shared text-style contract, service 8, scene 4, protocol
  minor 15, capability gate, controller surfaces.
- TV.2 (`7331a10`) — full-workspace Themes layout, view thumbnails.
- TV.1 (`0f1cf84`) — theme sample views through the preview transport.
