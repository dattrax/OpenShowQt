# OpenShowQt progress and handoff

Updated: 2026-11-04 (Europe/London).

## Current checkpoint

**TR.1 complete (2026-11-04): a cue's transition is decided when Go is pressed,
and it is the same answer all the way to the socket.**

Before this leaf the reveal was a kind (`fade`/`cut`) chosen near the draw, and
the numbers behind it were two constants in the renderer. Now the command owns
one atomic record — `domain::TransitionOptions{kind, textDurationMs,
mediaDurationMs}` — and the interesting part of this leaf is everything that had
to change for a *record* to survive a round trip through an asynchronous cue
lane.

**One sentence about values.** `Documents::validTransitionOptions` is the only
place the band exists (whole milliseconds 0..10000, defaults 500 ms words /
800 ms media), and the three verbs, the loader's era-gated read and the worker's
`scene.present` parse all quote it — so `10001` costs the same words at every
door and no door can accept what another would refuse. Each refusal names both
the operator's word and the wire key (`words duration ("textDurationMs") …`),
because a settings page quotes this sentence to a person and a socket quotes it
to whoever wrote the payload.

**Three seats, one walk.** One CONCRETE service default — there is no "unset
service default" for a resolver to invent, since the 500/800 every present
performed before this setting existed *is* its value — plus optional whole-record
overrides on `Theme` (the Songs view) and `TextContext` (the Bible view). The
walk follows the chain the typography already uses: the named context first (the
view that set the slide is the nearer answer about how that view moves), then
the FIRST theme in chain order (an explicit Cut stops the walk exactly like a
fade does — a decision, not an absence), then the service. Silence is not a
default: a theme that inherits reaches past the service to whatever is under it,
and a context that says nothing is not a zero-length fade. A slide the model
cannot name is not a refusal either — it answers with the service's record,
because the audience is owed a reveal either way.

**Service format 9.** The root record is written ALWAYS and REQUIRED (a 9-file
with no `"transition"` is not a package this build produced; guessing which fade
an unnamed key meant is how a document starts lying about its own output), the
theme's and the context's are optional like placement, and the key is
era-refused at all three sites in a pre-9 file — root, theme and context — where
it is another build's meaning of a key this writer never emitted. A silent
document writes exactly ONE `"transition"` (the root's), so existing documents
keep their shape, and re-saving is byte-identical.

**The record belongs to the Go, not to the ack.** `ScenePrepareAsk.transition`
is a value copy stamped by `armPrepare` through `Outbound::resolveTransition`,
and `handlePrepareAck` puts *that* record on `scene.present` and consults nothing
mutable — by the time an ack lands, "the settings" are whatever the operator has
moved on to, and the cue they already pressed is owed the fade it was promised.
Down the socket the whole record rides the change (`SceneStore::Snapshot.
transition`; a repeat ask for the scene already desired changes nothing and
cannot retime the reveal already owed — the R13 rule extended, not a new one),
and `OutputSession::presentScene(prepared, revisionOut, const
TransitionOptions&)` has NO default argument: a legacy entry point says so by
handing over `defaults()` in the open, because a present that quietly invented
one is how a queued reveal ends up timed by whatever the settings screen held
when the frame was built.

**The honest half: the capability is NOT advertised.** Protocol minor is **18**
and `scene.transition-options.v1` is defined, but deliberately absent from the
worker's hello, because this build's crossfade still runs on the library's own
0.5 / 0.8 s constants — the claim has to be earned by the renderer, not by the
parser, exactly as `scene.text-style.v1` waited for the stroke that actually
drew. So the landed handoff narrows the record to the renderer's vocabulary at
ONE named seam, `sceneTransitionKind()` in `render/Layer.h`, whose comment says
why, and `protocol_tests` asserts the ABSENCE so TR.2 flipping it is a
conscious edit.

**The gate downgrades; it does not refuse.** That is a deliberate departure from
the TS.1 style gate: an operator who pressed Go is owed the next cue, and a fade
that took the worker's own 500 ms instead of the asked-for 100 ms is a lesser
loss than a slide that did not come up. Toward a peer that has not claimed the
capability, the two keys are left OFF the payload (an old worker gets exactly
the legacy-shaped command it has always understood) and the status line says the
cue moves on the worker's own 500 ms / 800 ms fade instead of the pair asked
for. Silence is honoured in both directions — an unconfigured document sends
byte-for-byte the pre-TR.1 command and says nothing at all.

`scene.present`'s durations are vetted before the store is touched and whatever
the kind says (a cut-shaped exemption would let a malformed number land on a
scene that happens to cut): fractional, string, boolean, negative, >10000 and
no-millisecond-count numbers each refuse as invalid params naming their key,
and none of them moves the audience or queues anything.

Evidence: `domain_tests`, `scene_store_tests`, `protocol_tests`,
`rpc_ipc_tests` (ctest name `worker_rpc_lifecycle`),
`worker_controller_tests::scenarioTransitionIntent` (a scripted peer that holds
A's prepare ack 400 ms while the resolver moves 100/200 → 900/1000, with every
present's params logged VERBATIM: the wire says 100/200),
`document_controller_tests` (verbs, undo, refusals, the context override
outranking the theme wearing it through a real imported passage, duplication
carry, disk round-trip). Gates: desktop `ctest -j1` **65/65** (~384 s), headless
build + `ctest -j1` **36/36**, `git diff --check` clean.

Earlier this plan: TS.5 (`c3f5e84`) the pinned offline font corpus; TS.4
(`32bcdb7`) worker-owned `font.list` and one-field style controls; TS.3
(`c2e6645`) styled measurement (`ink`/`fillInk`, `text.measure.v2`); TS.2
(`e4258e9`) the styled renderer; TS.1 (`c5acad7`) the style contract (service 8,
scene 4, minor 15); TV.2 (`7331a10`) the Themes layout; TV.1 (`0f1cf84`)
sample-based previews.

## Where the work stands

Phases 0–5 complete. Phase 6 complete through 6.26i. The Bible plan (BA.1–BA.8)
is DONE. The themes plan: TV.1, TV.2, TS.1, TS.2, TS.3, TS.4, TS.5 and **TR.1**
DONE. Next is **TR.2** (the renderer draws the landed per-command durations,
documents the video limits, and finally earns
`scene.transition-options.v1`) and then **TR.3** (Settings and Theme Editor
transition UI). The TV.3/TV.4/TV.5/TA.1 leaves are out of this run by request.

## How to pick up

1. `PLAN.md` is the contract: ledger rows carry acceptance evidence, the themes
   runbook paragraphs give each leaf's boundary, and "Format allocation" records
   the current **9 / 4 / 0.18**.
   `docs/reviews/theme-editor-2026-11-03.md` is the design authority (its
   Transition contract section is TR's).
2. Build `cmake --build --preset desktop --parallel 2` (build dir `build/`);
   test `ctest --preset desktop -j1` (~6.5 min). Headless: `--preset headless`
   (`build-headless/`, 36 tests). Individual binaries: `./build/tests/<name>`;
   `text_measure_tests`, `worker_controller_tests` and `live_fade_tests` take
   the worker binary as argv[1]. ctest names are target-ish, not file-ish:
   `rpc_ipc_tests` runs as `worker_rpc_lifecycle`. Rebuild the worker binary
   after renderer/protocol changes — a stale one once faked a green suite (TR.1
   hit exactly that: the refusal-wording change looked unapplied until
   `openshow-worker` was rebuilt).
3. **TR.2 is the renderer leaf.** Read `src/worker/render/Layer.h` first: the
   enum, the narrowing seam and the comment saying what TR.2 replaces. The work
   is `SceneRenderer.cpp`'s `kTextFadeSeconds` / `kMediaFadeSeconds` becoming the
   landed record (it is already carried to `SceneTransition`-adjacent state via
   `Snapshot.transition`), plus cut / zero / unequal durations, first scene,
   rapid Go, clear/restore, failed presentation, the mirror using the same
   blends, and the same-background video-continues rule. Then ADD
   `kCapabilityTransitionOptions` to the worker's hello in
   `protocol.cpp`, flip `protocol_tests`' absence tripwire to a presence check,
   and cover the video-switch fade limitation the review names.
   `preview_budget_tests` keeps its own `kMediaFadeSeconds = 0.8` ruler, which
   stays valid only while the renderer's constants are the contract's defaults.
4. **TR.3 is the UI leaf.** The verbs already exist and are tested:
   `DocumentController::setServiceDefaultTransition`, `setThemeTransition`
   (empty map = inherit), `setTextContextTransition`, plus the reads
   `serviceDefaultTransition()`, `transitionForSlide(showId, slideId)` and
   `transitionForSceneJson(scene)` — the last is what a Go asks, so the number
   an operator reads and the number a cue moves with come from one walk.
   Edits must affect the NEXT Go, never an active fade; `main.cpp`'s
   `setTransitionResolver` wiring is where the documents meet the cue lane.
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
7. Wire-test lore this leaf: the scripted peer in `worker_controller_tests`
   writes a report per scenario and now logs every `scene.present` verbatim as
   `presentOps` (a count cannot tell 100/200 from 900/1000); scenario
   `transition-late` is the only way the "settings moved mid-flight" branch is
   reachable at all. A `boost::json` ledger member must be `boost::json::array`,
   not `std::vector<boost::json::object>` — the latter does not convert.
8. Never hold a `const Theme*` across a call that can grow the theme vector
   (`duplicateTheme` copies up front for exactly that reason). Qt6 has no
   `QMetaType::QMap`. A ctest "Passed" after a failed build means a stale
   binary — rebuild first, and grep piped builds for `error:|Error [0-9]`;
   multi-main UI tests print nothing on pass.

## Recent commits

- TR.1 — the transition contract landed to the socket: `domain::TransitionOptions`
  with one shared validator, service default + Theme/TextContext overrides,
  `resolveTransitionForSlide` (context → first theme in chain order → service),
  service format **9** with the root record required and pre-9-era refusals at
  all three sites, options captured at Go and carried as a value through the
  prepare intent to `scene.present`, `SceneStore`/`OutputSession` taking the
  whole record with one named narrowing seam at the landed handoff, protocol
  minor 18 with `scene.transition-options.v1` defined-but-withheld, and the
  duration gate that downgrades and speaks rather than stranding the audience.
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
