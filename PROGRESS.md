# OpenShowQt progress and handoff

Updated: 2026-11-16 (Europe/London).

This file is the CURRENT handoff. Earlier checkpoint narratives — the media
library, the type/effects review, the CS/TV/TS/TR series and the M6 milestones —
are archived verbatim in
[`docs/history/progress-archive-2026-11-16.md`](docs/history/progress-archive-2026-11-16.md);
git history is the authority for what each of those commits did. Nothing in the
archive is a current instruction.

## Current checkpoint

**The design-conformance corrections (DR.0–DR.8) are complete.** The review
[`docs/reviews/design-conformance-2026-11-15.md`](docs/reviews/design-conformance-2026-11-15.md)
found seven integration regressions in the media work that landed with
`054a1d0`. Each is fixed, with the regression pinned by a test that fails on the
reviewed commit:

| Leaf | Commit | What changed |
|---|---|---|
| DR.0 | `9f146fe` | The cue audio contract is recorded in `docs/architecture.md` ("Media audio: one contract, and when it restarts"): three lifetimes (cue layer + embedded video audio in one player; legacy `media.standalone`), the audible identity as the pair (embedded stream, show layer), continuity across slides and visual-only changes, Clear Background/Slide preserving the layer, Clear All stopping it, Restore starting it from the beginning, seek/loop on the player, mute/gain surviving rebuilds, and acknowledged show audio counting as output activity. |
| DR.1 | `7351d9b` | `withBackgroundCleared` is a complete transformation through the scene contract (video/camera/source/mirror/gradient all go), and `sceneContent` derives its flags from the same typed reader the worker uses. A video clear used to be refused by the codec; a camera clear used to come back on air. |
| DR.2 | `e408181` | One traversal (`SceneEdit::visitAssetPaths`) covers the background, every image item AND the scene 7 audio layer; the mirror, pending prepares, the Restore snapshot and the desired presentation all adopt a save's moved paths. |
| DR.4 | `da1960b` | The staged player and every flag describing it are one `StagedAudioBundle`, taken whole under one lock — no loose cross-thread booleans. A barrier test publishes B inside the take-hook window and proves A keeps its own identity. |
| DR.3 | `09be595` | Audio is staged for every background kind (audio-only, still, camera, video) and keyed by the complete identity; the on-air shortcut includes the layer; a scene whose prime is in flight defers once; a staged failure rides the bundle; a visual-only change keeps the player and its position. |
| DR.5 | `80c8259` | `SceneContent.audio` / `audioOnAir` count the cue's own layer as output activity for the panel's label, Clear All and the Escape gate, while words and background clears stay layer-scoped. |
| DR.6 | `f2ba7e3` | Plans capture and recheck catalog record/content revisions and the destination show; owed ids live in a deduplicated, catalog-bounded set drained one run per publish; a removed destination is dropped, never re-added. |
| DR.7a | `7864058` | `media.describe`/`media.thumbnail` run on their own bounded, cancellable executor and answer by deferred reply, so a slow bin file cannot hold Go/clear/status. |
| DR.7b | `afcee65` | Bin pixels are a worker-produced PNG in a bounded cache (64 files / 32 MB / 24 h, name fingerprints source mtime+size); the reply carries the path, the card loads a file URL, and a reply is installed only for the request this client made. |
| DR.8 | this commit | Journeys and suites re-run, architecture reconciled, superseded handoff narratives archived. |

**Verification (fresh, 2026-11-16):**

- `cmake --build build --parallel 24` — clean.
- `ctest --test-dir build --output-on-failure` — **85/85 passed** in 474 s,
  including the 21 `display`-labelled entries, the 10 architecture/dependency
  guards and their negative fixtures (`skia_public_api_guard`,
  `render_layer_guard`, `render_target_guard_negative`, `app_has_no_ndi_symbols`,
  `ui_links_qt`, `worker_check_deps`, `ui_loads_guard_*`).
- `cmake --build build-headless --parallel 24` then
  `ctest --test-dir build-headless -LE display` — **47/47 passed** in 81 s.
- Skips are the environment's, not the suite's: PipeWire-daemon and VA-API
  branches report internal skips when the sandbox denies the audio server or
  the GPU decoder, and `media_tests` skips its PipeWire check without a daemon.
  No test was skipped by label beyond the headless run's explicit exclusion.

**Still open (unchanged by DR, and not inferable from a green suite):** TV.3,
TV.4a, TV.4b, TV.5 and TA.1; ND.4's real source-loss acceptance; M7.1a–c,
M7.2–M7.4; the physical acceptance that needs the projector on a second display
(Wayland and X11), a live camera unplug, an audible pairing and a long mixed
service. FO stays gated behind those.

## How to pick up

1. `PLAN.md` is the contract; the DR group is closed and the remaining open
   leaves are named above. `docs/architecture.md` carries the reasoning the
   code is held to, including the DR.0 audio policy.
2. Build: `cmake --build --preset desktop --parallel 24` (`build/`); test:
   `ctest --preset desktop -j8` (~8 min). Headless: `--preset headless`
   (`build-headless/`, 47 entries, display excluded).
3. Facts worth keeping: a ctest "Passed" after a failed build means a stale
   binary — rebuild and grep for `error:`. `worker_controller_tests`,
   `rpc_ipc_tests` (`worker_rpc_lifecycle`) and `live_fade_tests` take the
   worker binary as argv[1] under ctest. Test fixtures are written into the CWD
   (`transport-fixtures/`, `audio-mix-fixtures/`, `playback-fixtures/`,
   `background-media-fixtures/`, `scene-render-*.png`) and are git-ignored.
4. Protocol/service/scene formats are now **protocol minor 19**, **service
   format 13**, **scene format 7**; `docs/architecture.md` and
   `docs/ui/control-sets.md` are the authorities for the wire and the controls.

## Recent commits

- `afcee65` fix(media): deliver Media Bin pixels as a worker-produced cache file (DR.7b)
- `7864058` fix(media): run Media Bin probe/decode work off the control connection (DR.7a)
- `f2ba7e3` fix(library): every linked refresh survives a delayed preparation (DR.6)
- `80c8259` feat(ui): count the show's audio layer as output activity (DR.5)
- `09be595` fix(media): stage audio for every background kind and keep it across visual changes (DR.3)
- `da1960b` fix(media): transfer the staged player and its metadata as one bundle (DR.4)
- `e408181` fix(scene): adopt every scene asset path, including the show audio layer (DR.2)
- `7351d9b` fix(scene): clear media backgrounds through the typed contract (DR.1)
- `9f146fe` docs(media): record the cue audio contract and its continuity rules (DR.0)
- `a10ed4b` docs: review design conformance and plan corrective work
- `054a1d0` feat: audio is a cue, camera mirroring, and the type/effects review
