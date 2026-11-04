# OpenShowQt progress and handoff

Updated: 2026-11-04 (Europe/London).

## Current checkpoint

**TS.3 complete (2026-11-04): one typography now answers measurement and
drawing, and the worker that can say so is the only one asked.** Layout,
`text.measure`, the text-flow engine and the Scripture importer all carry
the resolved style block; every measurement reply grows `fits` + `bounds`;
protocol minor 16 names `text.measure.v2`; and a styled question toward a
pre-16 worker is refused locally, by capability name, with nothing sent.

The two answer-shaped numbers divide honestly, and the division is the
design (`SceneText.h` says it at the struct): `LaidOutItem.ink` is where
any pixel may land — the fill's line-metrics box grown half a pen each
side by an enabled stroke and unioned with the offset copy grown by the
finite ~3.1σ blur support (Skia's real support measured ≈3.09σ; the code
claims 3.1 and the fixtures bound it from both sides) — while `fillInk` is
the pre-effect box and `fits` judges ONLY that. Why: an enabled shadow's
spill is the author's choice and no repagination shortens it, so folding
effects into the verdict would refuse every page of a theme that so much
as shadows its text. `fits=false` therefore arises where words genuinely
cannot live (the optional `height` key → `boxHeight` leg; SkParagraph
char-breaks a too-wide word so the fill stays inside and the words keep
their verdict — the promise "nothing clips, words move"). The new
`fitMode:"fixed"` branch lays out once at the authored size and answers;
the legacy shrink loop is untouched (pinned).

The wire (minor 16, `kCapabilityTextMeasureV2`, hello now 19): blocks may
carry `fontWeight`/`fontStyle`/`fitMode`/`minFontSize` and whole
packed-int-colour effect records — every band refused by name, split
`offsetX`/`offsetY` sentences, closed records reject unknown members by
name — and EVERY reply grows the additive `fits` +
`bounds{empty,left,top,right,bottom}` (v1 readers ignore; a v1-shaped ask
still gets them). The frontend gate is the honest half: `WorkerController`
packs hex→int (`attachMeasureStyle`) and refuses a style-DEVIATING batch
toward a peer that never named v2 — pre-16 workers silently ignore unknown
keys, so a bold-italic ask would come back answered in the default face,
a wrong page with no trace. Deviation = weight≠400, slant≠normal, stroke
enabled+width>0, shadow enabled+opacity>0; disabled-loud records and
spelled defaults travel (proved bit-identical measurements).

The flow (`src/importer/textflow.{h,cpp}`): `FlowStyle`/`MeasureRequest`
carry the domain's own `TextStroke`/`TextShadow` (importer links the
Qt-free `openshow_domain` — "identical inputs" is a type-level claim), the
style rides every ask verbatim, and `Answer{height,fits}` judges
`height <= available && fits != 0` — with fits=-1 (a pre-v2 answer)
leaving the height as the whole judgment, the pre-TS.3 floor honoured
rather than guessed over. `validateFlowStyle` vets the style bands (NaN σ
included) before the port is touched. `measureCacheKey` — one canonical
length-prefixed serializer over content, box AND the whole style — is
shared by the engine's per-run cache and `ScriptureImport`'s across-run
cache (a theme changed mid-session is a different question; the wire
correlation key is NOT part of it), capped at `kMaxMeasureCacheEntries`
4096. `DocumentController::scriptureBoxStyle` now reads the FULL style off
the compiled fixture's first text item — the very item the slide draws —
so packer and audience cannot drift by construction.

Evidence shape: the raster fixture (`testInkBoundsContainEveryEffectPixel`)
renders the 0.5-scale canvas and proves every drawn pixel lands inside
`ink` mapped through the device scale for fill / stroke / shadow /
negative-offset / both, plus the growth arithmetic (±half-pen; offset plus
3.0..3.1σ) and fixed-vs-shrink. The IPC suite pins spelled-defaults
bit-identical to absent, weight-800 measuring wider than 100, stroke
bounds at ±half-pen, shadow union bounds honest in both directions,
spill-still-fits, the char-break line, the height-leg verdict, and 15
by-name refusals. The flow fixtures pin the style riding every ask,
verdict-No refusing by its named dead end, the no-verdict floor packing
unchanged, and the cache-key field matrix. `worker_controller_tests` S8c
choreographs the gate closed (styled refused naming `text.measure.v2`,
ZERO `text.measure` frames on the socket, defaults answered with no
verdict row) and open (records arrive whole with colour packed,
verdict+bounds parse back), reconnect included.

Gates: desktop `ctest -j1` **64/64** (~378 s), headless build + `ctest
-j1` **35/35**, `git diff --check` clean.

Earlier this plan: TS.2 (`e4258e9`) shipped the styled renderer and the
advertised `scene.text-style.v1`; TS.1 (`c5acad7`) the style contract
across domain/storage/scene/protocol/controller (service format 8, scene
format 4, minor 15); TV.2 (`7331a10`) the Themes layout; TV.1 (`0f1cf84`)
the sample-based inspector previews.

## Where the work stands

Phases 0–5 complete. Phase 6 complete through 6.26i. The Bible plan
(BA.1–BA.8) is DONE. The themes plan: TV.1, TV.2, TS.1, TS.2, TS.3 DONE.
Next is **TS.4** — the installed font/face picker and the selected-view
typography/effect controls (family list from the worker's own FontCollection,
unavailable-family feedback, inherit/revert, validated stroke/shadow/fit
fields, one undo per edit/gesture; `workspace_ui_tests`). Then TS.5
(pinned Inter/Lora/Oswald offline raster/perf fixtures), then TR.1–TR.3
(transitions; TR.1 claims service format 9).

## How to pick up

1. `PLAN.md` is the contract: ledger rows carry acceptance evidence, the
   themes runbook paragraphs give each leaf's boundary, "Format allocation"
   records the current 8 / 4 / 0.16 numbers (TS.3 bumped minor to 16 and
   ADVERTISED `text.measure.v2` — the name was reserved with the version).
   `docs/reviews/theme-editor-2026-11-03.md` is the design authority.
2. Build `cmake --build --preset desktop --parallel 2` (build dir `build/`);
   test `ctest --preset desktop -j1` (~6.5 min). Headless: `--preset
   headless` (`build-headless/`, 35 tests). Individual binaries:
   `./build/tests/<name>`; `live_fade_tests` takes the worker binary as
   argv[1] (`./build/src/worker/openshow-worker`). Rebuild the worker
   binary after renderer/protocol changes — a stale `openshow-worker` once
   faked a green `live_fade_tests`.
3. TS.4 starts in the UI: the theme/workspace QML and `DocumentController`
   verbs (TS.1 already shipped `setThemeTextStyle` and the `...Set`
   badges), plus a font-listing seam — the worker is the only honest
   answerer of "which families resolve", and TS.2's face resolution
   (`setFontStyle` through the FontCollection) is what a picker must match.
   The measure seam is now styled end to end: `scriptureBoxStyle` →
   `FlowStyle` → `measureText` → v2 blocks, so editor probes can reuse it
   for styled fit read-outs.
4. Measurement lore, still true: the blur sigma is device-space (the
   painter rescales), stroke width is CTM-scaled by Skia, the fork's
   shadow pass fires `drawTextShadow` once per run BEFORE the blobs. New:
   `LineMetrics` fields are `fLeft/fWidth/fBaseline/fAscent/fDescent`
   (`modules/skparagraph/include/Metrics.h`); Skia's blur support is
   ~3.09σ and the code's 3.1 claim is fixture-bounded from both sides;
   SkParagraph CHAR-breaks words wider than the box (fill stays inside,
   lineCount≥2 — the width leg of `fits` is unreachable in practice, the
   height leg is where verdicts arise); fixture renders are 960×540 = the
   authored canvas at 0.5, so containment checks map ink through that
   scale with 2 px slack.
5. Boost 1.90 spells doubles in exponent form; manifest assertions match
   style keys by NAME. JSON fixtures use decimal integers. Never hold a
   `const Theme*` across a call that can grow the theme vector. Qt6 has no
   `QMetaType::QMap`. A ctest "Passed" after a failed build means a stale
   binary — rebuild first; multi-main UI tests print nothing on pass.

## Recent commits

- TS.3 — styled measurement end to end: `ink`/`fillInk` split, `fits`+
  `bounds` replies, `fitMode:"fixed"`, minor 16 + `text.measure.v2` gate,
  shared full-style cache keys, flow verdict criterion.
- TS.2 (`e4258e9`) — faces/stroke/shadow through public Skia, fade-aware
  painter, measured device-space blur rescale, `scene.text-style.v1`
  advertised, raster + GPU-live fixtures.
- TS.1 (`c5acad7`) — shared text-style contract, service 8, scene 4,
  protocol minor 15, capability gate, controller surfaces.
- TV.2 (`7331a10`) — full-workspace Themes layout, view thumbnails.
- TV.1 (`0f1cf84`) — theme sample views through the preview transport.
