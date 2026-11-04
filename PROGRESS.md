# OpenShowQt progress and handoff

Updated: 2026-11-04 (Europe/London).

## Current checkpoint

**TR.2 complete (2026-11-04): the audience's fade is measured in the milliseconds
the cue carried, and the worker finally claims it.**

TR.1 got the record to the socket intact; TR.1's own honesty note was that the
pixels did not listen — the renderer owned two constants and read only the kind.
That seam is now gone, and what is interesting about this leaf is how little new
machinery drawing the record needed once the record existed, and how many
near-misses the fixtures caught in the last millisecond of it.

**No narrowing left.** `SceneRenderer::setActiveScene(ActiveScene, const
domain::TransitionOptions&)` — no default argument — takes the whole answer,
`OutputSession`'s landed handoff passes `snapshot.transition` across undivided,
and `SceneTransitionKind` plus `sceneTransitionKind()` are RETIRED from
`render/Layer.h`: a record narrowed on the way is a promise dropped on the way,
and there is nothing left to translate into. `kTextFadeSeconds` /
`kMediaFadeSeconds` are deleted. The two entry points that have no command behind
them — the development harness's demo scenes, the snapshot CLI — say
`TransitionOptions::defaults()` in the open, which is exactly why the signature
has no default: a present that quietly invented a record is how a queued reveal
ends up timed by whatever the settings screen held when the frame was built.

**One conversion, at land time.** `SceneTransition` stores `fTextSeconds` and
`fMediaSeconds`, divided once when the cue lands rather than per frame:
`captureFrame` runs at the panel's rate, and a divide there would be paid sixty
times a second for two numbers that cannot change while the fade runs. A record
belongs to its cue, so nothing that happens after it is stamped can retime it.
Where a fallback is genuinely needed (entry points with no record) the numbers
come from the domain's `kDefaultTransitionTextMs` / `kDefaultTransitionMediaMs`,
not a second pair of constants that a later leaf could forget to move.

**The order of two guards is an acceptance criterion.** `fadeBlend` tests
`duration <= 0` BEFORE `elapsed <= 0`. A 0 ms track is an answer the operator
gave, and a fade's first captured frame has elapsed 0.0 — an elapsed-first test
would return 0 there and show the outgoing scene to someone who asked for nothing
to fade: a 500/0 cue's words arriving a frame late, a 0/0 fade taking a frame to
do a cut's job.

**What the fixtures pin.** `scene_frame_tests::testDurationsComeFromTheCue`
measures milliseconds rather than labels: 0/0 is the settled picture on its very
first frame (and still owes the present that retires it); 0/800 has the outgoing
word *gone* from a frame whose background is still crossing; 500/0 lands the
colour with the outgoing word still sitting on it — the two tracks are not one;
100/200 reads as half the words and a quarter of the background at 50 ms, in
agreement between panel and preview, over at 200 ms not 800, and byte-identically
idle a minute later. A 50 ms cue interrupting an 800 ms one is over in 50 ms,
restarted from the scene that was *coming up* (BETA, not ALPHA) with no
half-fade composite in it. And the default record is still 500/800 to the
millisecond — 0.799 s is still mid-fade — so a service that never opens the
settings page keeps its exact pixels.

**Video is stated, not sold.** The background layer still takes the media cut
whenever video is involved in either direction: a crossfade there would composite
two still images over decoders that have not produced frames. So no media
duration buys a dissolve between clips (`testVideoBackgroundNeverDissolves`
aims a 10 s background at another video and finds the new background already
there — with both words on that same frame, because the text transition was
never the limitation), while a cue that keeps the same video background keeps
playing it continuously, which `background_media_tests` holds from the
transport's side.

**The two frontend rules, read off the wire.** Every explicit clear presents
`transition:"cut"`: a clear is not a cue, and taking something *off* the output
does not crossfade for the cleared slide's own 200 ms. Each clear also captures
the record its retained content is owed (`fRestoreTransition`, resolved from
THAT payload at the moment of retention, retired together with `fRestoreJson`),
so `restoreOutput()` moves with 100/200 when the setting has since moved to
900/1000 — the slide coming back is owed the move it had before it was cleared
away, not whatever theme the operator happens to be browsing.
`worker_controller_tests::scenarioTransitionIntent` now proves all three presents
verbatim: go 100/200 fade, clear cut, restore 100/200.

**The name is earned.** `scene.transition-options.v1` is in the hello (21
capabilities), `protocol.h`'s minor-18 prose says what earns it, and
`protocol_tests` reads the other way now — the tripwire TR.1 left flipped to a
presence check, with the count pinned at 21 in both `protocol_tests` and
`rpc_ipc_tests`. No version moved for this leaf: the wire is exactly TR.1's and
only the pixels changed. Retirement is unchanged (the outgoing scene goes when
the settled frame reaches the panel, and a preview draw still cannot move the
clock), and the downgrade path is intact — toward a pre-18 worker or a
capability-less scripted peer the durations stay off the socket and the status
line names both clocks.

Evidence: `scene_frame_tests`, `worker_controller_tests`, `protocol_tests`,
`rpc_ipc_tests` (ctest name `worker_rpc_lifecycle`), `render_scene_transition`,
`scene_render_tests`, `preview_budget_tests` (its 0.8 s ruler is now the
contract's default rather than a renderer private), `live_fade_tests`,
`background_media_tests`. Gates: desktop `ctest -j1` **65/65** (~384 s), headless
build + `ctest -j1` **36/36**, `git diff --check` clean.

Earlier this plan: TR.1 (`108b919`) the transition contract landed as one record;
TS.5 (`c3f5e84`) the pinned offline font corpus; TS.4 (`32bcdb7`) worker-owned
`font.list` and one-field style controls; TS.3 (`c2e6645`) styled measurement
(`ink`/`fillInk`, `text.measure.v2`); TS.2 (`e4258e9`) the styled renderer; TS.1
(`c5acad7`) the style contract (service 8, scene 4, minor 15); TV.2 (`7331a10`)
the Themes layout; TV.1 (`0f1cf84`) sample-based previews.

## Where the work stands

Phases 0–5 complete. Phase 6 complete through 6.26i. The Bible plan (BA.1–BA.8)
is DONE. The themes plan: TV.1, TV.2, TS.1, TS.2, TS.3, TS.4, TS.5, TR.1 and
**TR.2** DONE. Next is **TR.3** — the Settings and Theme Editor transition UI,
the last leaf of the TR group (the TV.3/TV.4/TV.5/TA.1 leaves are out of this run
by request).

## How to pick up

1. `PLAN.md` is the contract: ledger rows carry acceptance evidence, the themes
   runbook paragraphs give each leaf's boundary, and "Format allocation" records
   the current **9 / 4 / 0.18** (TR.2 claimed nothing — it changed pixels).
   `docs/reviews/theme-editor-2026-11-03.md` is the design authority (its
   Transition contract section is TR's).
2. Build `cmake --build --preset desktop --parallel 2` (build dir `build/`);
   test `ctest --preset desktop -j1` (~6.5 min). Headless: `--preset headless`
   (`build-headless/`, 36 tests). Individual binaries: `./build/tests/<name>`;
   `text_measure_tests`, `worker_controller_tests` and `live_fade_tests` take
   the worker binary as argv[1]. ctest names are target-ish, not file-ish:
   `rpc_ipc_tests` runs as `worker_rpc_lifecycle`. Rebuild the worker binary
   after renderer/protocol changes — a stale one once faked a green suite.
3. **TR.3 is the UI leaf, and every verb it needs already exists and is tested**:
   `DocumentController::setServiceDefaultTransition`, `setThemeTransition`
   (empty map = inherit), `setTextContextTransition`, and the reads
   `serviceDefaultTransition()`, `transitionForSlide(showId, slideId)`,
   `transitionForSceneJson(scene)` — the last is what a Go asks, so the number an
   operator reads and the number a cue moves with come from one walk. Put service
   defaults in Settings and a Transition section in the Theme Editor: Inherit /
   Cut / Crossfade for view overrides, showing the inherited source and the
   resolved words/background milliseconds, duration fields disabled for a Cut
   WITHOUT erasing them, and the video-background limitation stated beside the
   background control (the renderer's bypass is documented at
   `SceneRenderer.cpp`'s BackgroundLayer and in `scene_frame_tests`). Songs
   writes the Theme record, Bible writes its context.
   `main.cpp`'s `setTransitionResolver` wiring is where the documents meet the
   cue lane, so an edit moves the NEXT Go by construction — the acceptance is to
   prove it does not touch an active fade, and that a Go pressed after the edit
   really changes landed frame timing (`workspace_ui_tests` for the gestures,
   `document_present_tests` / `production_journey_tests` for the output).
   Gesture discipline from TS.4 applies: sliders commit on release, a commit of
   an unchanged value writes nothing and costs no undo, a refused write re-reads
   and snaps back.
4. Renderer lore this leaf: `captureFrame` stamps the fade's origin on its FIRST
   captured frame, so a fixture that asserts a blend at +0.05 s must take an
   origin capture first (`testDurationsComeFromTheCue` does); a fixture that
   forgets gets blend 0 at every clock reading and "fixes" nothing. `frame.
   fadeFrom` is what says a frame is a blend at all — an idle frame carries no
   outgoing scene whatever the durations were.
5. C fixture lore, still true: the font corpus is a prepared cache, not vendored
   (`tools/setup-font-fixtures.sh prepare --download`, then
   `ctest --preset desktop -R font_fixture_tests`; without it the test reports
   `Skipped`, which is the designed outcome). `curl` needs `--globoff` for those
   filenames; Boost 1.90 spells doubles in exponent form and JSON fixtures use
   decimal integers; `-fno-rtti` is required on any TU that includes Skia
   headers; scene JSON colours are packed integers, not `#rrggbb` strings; the
   snapshot CLI is `build/src/worker/openshow-worker-dev --snapshot out.png
   --scene-file s.json` at 1920×1080 for the 960×540 authored scene.
6. UI test lore, still true: `QMetaObject::invokeMethod(item, "toggle")` flips
   `checked` but does NOT fire `onToggled` — drive checkboxes with a real
   `clickAt`. A closed `Popup` is not a `QQuickItem`: find it with
   `window->findChild<QObject*>("name")`. `console.warn`/`log` from QML is
   invisible here. QML changes only reach the binary through a FULL
   `cmake --build --preset desktop`. Flickable `contentY` assignments are not
   clamped. In `workspace_ui_tests`, `docs.status()` is overwritten by an
   autosave refusal planted earlier — never assert a refusal through `status()`
   there.
7. Wire-test lore, still true: the scripted peer in `worker_controller_tests`
   writes a report per scenario and logs every `scene.present` verbatim as
   `presentOps` (a count cannot tell 100/200 from 900/1000); scenario
   `transition` advertises the capability and answers inline, `transition-late`
   holds the first prepare's ack 400 ms, and `clean` says nothing — which is the
   only way the downgrade branch is still reachable now that the real worker
   claims the name. A `boost::json` ledger member must be `boost::json::array`,
   not `std::vector<boost::json::object>` — the latter does not convert.
   Capability-count tripwires live in BOTH `protocol_tests` and `rpc_ipc_tests`.
8. Never hold a `const Theme*` across a call that can grow the theme vector
   (`duplicateTheme` copies up front for exactly that reason). Qt6 has no
   `QMetaType::QMap`. A ctest "Passed" after a failed build means a stale
   binary — rebuild first, and grep piped builds for `error:|Error [0-9]`;
   multi-main UI tests print nothing on pass.

## Recent commits

- TR.2 — the renderer draws the cue's own record: `setActiveScene` takes
  `TransitionOptions` with no default argument, `SceneTransition` carries both
  tracks' seconds converted once at land time, `fadeBlend` checks `duration <= 0`
  before the elapsed rule, the library's fade constants and the fade/cut enum and
  narrowing seam are retired, explicit clears present a Cut and a restore moves
  with the record captured beside its retained content, video keeps taking the
  media cut (documented and tested in both directions), and
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
