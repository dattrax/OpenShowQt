# OpenShowQt progress and handoff

Updated: 2026-11-04 (Europe/London).

## Current checkpoint

**TR.3 complete (2026-11-04): the operator types a number and the audience feels
it — measured in frames, not in a widget's `text`.**

The transition contract was two leaves of plumbing: TR.1 got the record to the
socket intact and TR.2 got the renderer to obey it. Both were honest that nobody
could *set* one. This leaf opens the two doors and then refuses to call that
proof until a real worker's preview frame was timed, because the acceptance says
out loud not to infer success from a widget value.

**One author for the shape, so a read cannot offer what a write refuses.**
`DocumentViews::transitionRecordMap(optional<TransitionOptions>)` is the single
place a record becomes QML — `{kind, textDurationMs, mediaDurationMs}`, or an
EMPTY map for a view that says nothing, because there is no half-record default
to invent. `insertThemeTransitionRow()` puts `transitionSet` beside `transition`
on every `themes()` row AND on `currentTheme()`, and `themeContext()` carries the
pair for the named context. `DocumentController`'s private `transitionToMap` is
deleted rather than kept beside it: two spellings of one record is how a page
ends up showing a key its own verb would turn down.

**Movement travels with the entry it belongs to.** `applyThemeContext` REPLACES a
`TextContext`, so it now carries the existing entry's `transition` across when
`fields` names no movement. Absent keys are silence about the field they name —
and a caller that came to change the reading font named nothing about how the
view arrives. Without that carry, editing a passage's font would have taken that
view's Crossfade off in silence, which is the kind of loss a document spends
years explaining. `transition` is therefore the ONE field whose absent keys do
not silence it, and `DocumentController.h` says so where the next hand will read
it.

**Three choices and no fourth, in both doors.** Settings holds the service's
standing answer; the Theme Editor's Songs section writes the Theme's record and
its Bible section the `scripture` context's. Inherit *writes nothing* — the
removal — and is enabled only against a record that exists, because a button
writing "nothing" over nothing costs an undo for no change. Cut and Crossfade
each write a WHOLE record seeded from what was on show: the file, the wire and
the domain all refuse a fragment, so one gesture is one undo and a half commit is
not something a page can do by accident. A Cut **disables** the two durations
without erasing them, so coming back to Crossfade comes back to the times that
were there. A silent view shows what silence resolves to — the page reads the two
records in front of it (`context → theme → service`, the order a Go walks) rather
than re-walking a chain, so it cannot disagree with `transitionForSlide` — and
the source line names the seat that answers ("inherits this theme — the theme
carrying this view answers before the service") and says a Cut on its own terms
instead of quoting times it ignores. Validation splits where the knowledge lives:
QML checks SHAPE only (digits, no sign, no point — anything else is `-1`, never a
silently-rounded `0`), and the 0..10000 band stays `Documents::validTransitionOptions`'
one sentence, quoted by the settings refusal and by the worker's parse;
`document_present_tests` proves the same `10001` earns the same words over a real
socket. A refused or non-numeric commit re-reads the model, and an unchanged
commit writes nothing.

**The pixels were the second half of the acceptance, and they taught two things.**
`frontend_journey_tests` leg 10 drives `DocumentController` verbs through
`main.cpp`'s own resolver wiring to a real worker and times the operator's
preview: a service default of 2600 ms is seen as an early blend AND a landing no
earlier than 1700 ms; the view's Cut shows no frame holding both grounds and
lands in a fraction of the same slide's crossfade; changing the view mid-fade does
**not** shorten the fade already crossing while the very next cue is a Cut; an
explicit `clearAll` reaches black without crossing whatever the view's numbers
say. And the restore is where the fixture had to learn the contract instead of
restating it — `PresentationModel` resolves the record its retained content is
owed at the moment the CLEAR takes its snapshot, so clearing a crossing cue and
*then* cutting the view still restores the words over 2.6 s. The claim in the
test is now the truth in the code: a retained restore point owns its own record,
and an edit made afterwards changes nothing.

**Two clocks, one lesson.** `scenePending` sleeps when the present's *reply*
lands; `sceneActive` moves when the next status poll reports it, every 500 ms. The
"editing a theme nobody wears presents nothing" claim failed honestly for exactly
that reason: a baseline taken at the first quiet moment is the cue BEFORE the
operator's last one, so the fixture now waits for the acknowledged revision to
hold still for longer than a poll before freezing it. A second lesson from the
same leg: a repeat present of a revision already on air is a no-op by design, so
every timing claim mints its own revision — a no-op that reports the picture it
was already showing is evidence of nothing.

**The last clause, taken literally.** "Restart begins black" is measured, not
asserted: the leg shuts its worker down and brings a fresh one up in the same
frontend. Its own preview is uniformly black with no words and no ground on air,
its hello still claims `scene.transition-options.v1` (the contract belongs to the
build, not to a process that has seen tonight's cues), and the same slide still
answers 2.6 s — the milliseconds live in the document. (`production_output_tests`
keeps owning the audience's every-pixel black, footer band included; this is the
frontend's preview of the same fact.)

Evidence: `workspace_ui_tests` (Settings' kind/numbers/disable-without-erase/undo;
the Songs picker refusing Inherit until a record exists, Cut seeding from what was
shown, typing one duration keeping its neighbour, an unchanged re-commit costing
no undo, `10001` refused with the record unmoved, Inherit surrendering a record
and one undo bringing picker and record back; the Bible door showing the THEME's
90/800 with "inherits this theme" for a silent view, writing its own record,
naming itself as the source, keeping its Cut through a `contextSize` commit, and
handing movement back without touching the reading size — the fixture leaving
theme, context, view and window as it found them), `document_present_tests` (the
resolver's own answer for a UI-written theme goes on `scene.present` and LANDS; a
key-less present is still the 500/800 a pre-TR.1 frontend performs; a Cut carries
its durations whole; an over-band number is refused naming key and range with the
audience unmoved; an inherited theme resolves back to the service's pair and the
worker accepts it on the same keys), `document_controller_tests` (rows and
`currentTheme` carry the record, silence reads EMPTY, a typography-only write
preserves the movement, one undo per commit with a refusal costing none),
`frontend_journey_tests` leg 10 (a)–(h) above. Gates: desktop `ctest -j1`
**65/65** (~410 s), headless build + `ctest -j1` **36/36**, `git diff --check`
clean.

Earlier this plan: TR.2 (`b3f6dc4`) the renderer drawing the cue's own record and
the capability earned; TR.1 (`108b919`) the transition contract to the socket;
TS.5 (`c3f5e84`) the pinned offline font corpus; TS.4 (`32bcdb7`) worker-owned
`font.list` and one-field style controls; TS.3 (`c2e6645`) styled measurement
(`ink`/`fillInk`, `text.measure.v2`); TS.2 (`e4258e9`) the styled renderer; TS.1
(`c5acad7`) the style contract (service 8, scene 4, minor 15); TV.2 (`7331a10`)
the Themes layout; TV.1 (`0f1cf84`) sample-based previews.

## Where the work stands

Phases 0–5 complete. Phase 6 complete through 6.26i. The Bible plan (BA.1–BA.8)
is DONE. The themes plan: **TV.1, TV.2, TS.1–TS.5, TR.1, TR.2 and TR.3 DONE** —
the run this session was asked for ("Text styling and effects" and
"Configurable slide transitions") is closed, and the TR group with it. Remaining
in the plan but out of this run by request: TV.3, TV.4a, TV.4b, TV.5 and the
closing leaf TA.1 (architecture and captures sweep).

2026-11-04 planning update: PLAN.md now records the **CS shared Qt control-set
work before FO**, including themed Bible previews, common appearance controls
and a new preview row for each running-order entry. CS.0 is a source review
only; CS.1–CS.9 are TODO. **Next action: CS.1**, defining part contracts and the
component catalogue. Complete the remaining TV/TA and M7 gates before FO. No
application behavior changed in this planning update.

## How to pick up

1. `PLAN.md` is the contract: ledger rows carry acceptance evidence, the themes
   runbook paragraphs give each leaf's boundary, and "Format allocation" records
   the current **9 / 4 / 0.18** (TR.2 and TR.3 claimed nothing — one changed
   pixels, the other opened doors onto records that were already stored).
   `docs/reviews/theme-editor-2026-11-03.md` is the design authority.
2. Build `cmake --build --preset desktop --parallel 2` (build dir `build/`);
   test `ctest --preset desktop -j1` (~7 min). Headless: `--preset headless`
   (`build-headless/`, 36 tests). Individual binaries: `./build/tests/<name>`;
   `text_measure_tests`, `worker_controller_tests` and `live_fade_tests` take the
   worker binary as argv[1] (running a UI binary straight from `build/tests`
   without ctest's environment fails at "no worker", which is the fixture, not a
   regression). ctest names are target-ish, not file-ish: `rpc_ipc_tests` runs as
   `worker_rpc_lifecycle`. Rebuild after renderer/protocol changes — a stale
   binary once faked a green suite.
3. **The transition surfaces, all of them, in one place**: the verbs
   `setServiceDefaultTransition`, `setThemeTransition` (empty map = inherit),
   `setTextContextTransition`; the reads `serviceDefaultTransition()`,
   `transitionForSlide(showId, slideId)`, `transitionForSceneJson(scene)` (what a
   Go asks, wired in `main.cpp`'s `setTransitionResolver`), and the row shape from
   `DocumentViews::transitionRecordMap`. The controls are SettingsPage.qml's
   TRANSITION section and ThemeEditorPage.qml's `themeEditorTransition*` (Songs)
   and `themeEditorContextTransition*` (Bible) controls, whose sync/commit
   functions are `syncTransition` / `commitThemeTransitionView` /
   `commitContextTransitionView`. Nothing there re-implements priority: the page
   reads records, the resolver walks the chain.
4. Transition lore this leaf earned, worth keeping:
   * `scenePending` (reply) and `sceneActive` (500 ms status poll) are **not** the
     same clock; sample an acknowledged baseline only after it holds still longer
     than a poll.
   * A repeat present of an already-active revision is a no-op, so a timing claim
     needs a fresh revision every time (`addSlide()` + `applyThemeToSlide` for a
     second payload).
   * White-pixel counting is unreliable across a fade OUT (it stops counting below
     α≈0.88) but fine across a fade IN; `uniformlyBlack` only trips near α≈0.96,
     which is what makes `msToBlack` a clean Cut-vs-2.6 s discriminator.
   * The restore's record is resolved at CLEAR time (`fRestoreTransition` beside
     `fRestoreJson`), not at Go time and not at restore time.
   * A second `WorkerController` in one process reuses the pid-named socket path
     safely: the exiting worker unlinks it. That is how leg 10 restarts a worker.
5. QML test lore, still true and now sharper: drive a `ComboBox` with
   `setProperty("currentIndex", n)` **and** `invokeMethod(item, "activated",
   Q_ARG(int, n))`; drive `CheckBox`/`PanelButton` with a real `clickAt`; a control
   at the bottom of the inspector rail must be scrolled to
   (`scrollInspectorTo` / `scrollInspectorToTop`) before it is clicked — clicking
   where it is *not* silently hits the wrong item, and forcing `enabled` on a
   control the model disabled tests a state the product never has. A closed
   `Popup` is not a `QQuickItem` (`window->findChild<QObject*>`). Flickable
   `contentY` is not clamped. In `workspace_ui_tests` there is no worker
   (`outputLive()` false, `sceneActive()` 0) and `docs.status()` is overwritten by
   an autosave refusal planted earlier — never assert a refusal through `status()`
   there; and an empty `transition` map means silence, never a filled-in default.
   QML changes only reach the binary through a FULL `cmake --build --preset desktop`.
6. Wire-test lore, still true: `scene.present`'s keys are `transition`,
   `textDurationMs`, `mediaDurationMs`, vetted by `parsePresentTransition` before
   the store is touched, and every refusal quotes
   `Documents::validTransitionOptions`. The scripted peer in
   `worker_controller_tests` logs every present verbatim as `presentOps` (a count
   cannot tell 100/200 from 900/1000) and capability-count tripwires live in BOTH
   `protocol_tests` and `rpc_ipc_tests` (21).
7. C fixture lore, still true: the font corpus is a prepared cache, not vendored
   (`tools/setup-font-fixtures.sh prepare --download`, then
   `ctest --preset desktop -R font_fixture_tests`; without it the test reports
   `Skipped`, which is the designed outcome). `curl` needs `--globoff` for those
   filenames; Boost 1.90 spells doubles in exponent form and JSON fixtures use
   decimal integers; `-fno-rtti` is required on any TU that includes Skia
   headers; scene JSON colours are packed integers, not `#rrggbb` strings; the
   snapshot CLI is `build/src/worker/openshow-worker-dev --snapshot out.png
   --scene-file s.json` at 1920×1080 for the 960×540 authored scene.
8. Never hold a `const Theme*` across a call that can grow the theme vector. Qt6
   has no `QMetaType::QMap`. A ctest "Passed" after a failed build means a stale
   binary — rebuild first, and grep piped builds for `error:|Error [0-9]`;
   multi-main UI tests print nothing on pass. Leg budgets worth knowing:
   `frontend_journey_tests` ~46 s of its 180 s TIMEOUT, `workspace_ui_tests` ~23 s,
   `document_present_tests` ~2 s of 90 s.

## Recent commits

- TR.3 — the transition UI with the audience as witness:
  `DocumentViews::transitionRecordMap` as the one author of the record's QML
  shape (`transitionSet` + `transition` on `themes()` rows, `currentTheme`,
  `themeContext`), `applyThemeContext` carrying the entry's movement across a
  replacing write, Settings' TRANSITION section, the Theme Editor's Transition
  section for Songs and for the Bible context (Inherit writes nothing and is only
  offered against a record that exists, Cut/Crossfade write whole records,
  durations disabled without being erased, the inherited source named, the
  video-background limit stated beside the background duration), QML validating
  shape only while the domain owns the 0..10000 band, and evidence in
  `workspace_ui_tests` (real gestures), `document_present_tests` (the record the
  controls wrote lands on a real socket) and `frontend_journey_tests` leg 10
  (timed pixels: default inheritance, view override, active-fade stability,
  off-air edits cost nothing, clear is a Cut, restore keeps the snapshot's record,
  ack-backed live markers, persistence, and a restarted worker streaming black).
- TR.2 (`b3f6dc4`) — the renderer draws the cue's own record: `setActiveScene`
  takes `TransitionOptions` with no default argument, `SceneTransition` carries
  both tracks' seconds converted once at land time, `fadeBlend` checks `duration
  <= 0` before the elapsed rule, the library's fade constants and the fade/cut
  enum and narrowing seam are retired, explicit clears present a Cut and a
  restore moves with the record captured beside its retained content, video keeps
  taking the media cut (documented and tested in both directions), and
  `scene.transition-options.v1` joins the hello at 21 capabilities.
- TR.1 (`108b919`) — the transition contract landed to the socket:
  `domain::TransitionOptions` with one shared validator, service default +
  Theme/TextContext overrides, `resolveTransitionForSlide` (context → first theme
  in chain order → service), service format **9** with the root record required
  and pre-9-era refusals at all three sites, options captured at Go and carried
  as a value through the prepare intent to `scene.present`, protocol minor 18
  with the capability defined-but-withheld, and the duration gate that downgrades
  and speaks rather than stranding the audience.
- TS.5 (`c3f5e84`) — pinned offline font corpus (manifest + setup script + hash
  checks), `font_fixture_tests` over an isolated fontconfig configuration (family
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
