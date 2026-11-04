# OpenShowQt progress and handoff

Updated: 2026-11-04 (Europe/London).

## Current checkpoint

**TS.2 complete (2026-11-04): the renderer draws the scene 4 style block,
and the worker now says so.** Weight and slant resolve through the font
face, the stroke paints behind the fill from the shaped blob, the shadow
follows the very same run, and all three ride the frame's fade alpha
exactly once — with the contract's defaults and disabled effects still
producing the pre-scene-4 pixels byte for byte.

How each part reaches the canvas matters, so it is written down where the
code lives (`src/worker/render/SceneText.cpp`). Faces enter shaping through
the public `TextStyle::setFontStyle(weight, 100, slant)`, so the
FontCollection picks the bold/italic FACE (or synthesises) exactly like a
browser. The shadow rides `TextStyle::addShadow`: the pinned fork paints
background → shadow → blobs → decorations, and its shadow pass hands the
painter a copy of the SHAPED run — so shadow and fill share wrap and
position by construction, never a re-wrap. Opacity folds into the shadow
colour's alpha once. The stroke CANNOT ride the style (one style paints one
pass; "outline behind fill" is two passes of one blob), so `GroupAlphaPainter`
gained a per-item stroke state and draws outline-then-fill from the same
`drawTextBlob` call's blob at the same position. Two Skia facts were
MEASURED against the pinned build, not assumed: a blur mask-filter sigma is
DEVICE-space (the same sigma spreads the same pixels at canvas scales 1.0
and 0.5 while glyphs halve — so the painter multiplies the authored sigma
by the canvas scale, memoised on the last sigma), while stroke widths ride
the CTM (the authored width passes through). `drawLayout` routes through
the painter whenever the fade is below full OR any text item carries an
effect, and keeps the old `Paragraph::paint(canvas…)` fast path byte-for-byte
otherwise. No `saveLayer`, no render target — the guards stay green.

The hello now advertises `scene.text-style.v1` (18 capabilities; protocol
minor stays 15, which RESERVED the name), and the protocol_tests absence
tripwire consciously flipped to presence. `WorkerController`'s gate is
unchanged and still earns its keep: a peer that names nothing still gets
non-default scenes refused, which is exactly what a pre-TS.2 worker is.

Evidence shape: the raster fixture (`testTextStyleFacesAndEffectsRender` in
`scene_render_tests`) pins loud-but-disabled effects byte-identical to none;
bold/italic sharing the regular wrap and band centres (+32% measured bold
ink); a 28-unit stroke growing ink by half its scaled width ≈7 px per side
with the red band provably outside the fill's own edge and the fill keeping
≥90% of its white pixels; the hard cyan shadow landing exactly at
fill.right + offset×scale with unchanged top/bottom; blur spreading (9967 vs
4774 cyan px) and diluting (peak g 157 vs 255); opacity 0.5 compositing
half-bright; and the 50% fade measured on regions painted by exactly ONE
part each — the single-glyph fill, the outline band beside the fill's edge,
the shadow zone right of it — giving ratios 0.50/0.50/0.50. (The first cut
measured the whole styled word and read 0.88 for fill: a legitimate STACK
of fill@½ over stroke@½ over doubled glyph-seam stroke, which says nothing
about how often the fade rode a part — single-painter regions are the
contract's claim.) The GPU live path (`live_fade_tests`) settles a styled
scene with 5892 stroke-red / 3762 shadow-cyan / 10171 white preview pixels,
lands clean to black, and carries each effect's masked mean through the
middle of its ramp across four separate frames — a stroke ignoring the fade
would cliff to zero at the end, a double fold would leave the window early.

Gates: desktop `ctest -j1` **64/64** (378 s; includes `unicode_text_tests`,
`scene_frame_tests`, `live_fade_tests`, `skia_public_api_guard`,
`render_layer_guard`, and the still-closed `worker_controller_tests` gate
scenario), headless build + `ctest -j1` **35/35**, `git diff --check` clean.

Earlier this plan: TS.1 (`c5acad7`) shipped the style contract across
domain/storage/scene/protocol/controller (service format 8, scene format 4,
protocol minor 15, capability gate, the `duplicateTheme` dangling-pointer
fix); TV.2 (`7331a10`) the full-workspace Themes layout with view
thumbnails; TV.1 (`0f1cf84`) the sample-based inspector previews.

## Where the work stands

Phases 0–5 complete. Phase 6 complete through 6.26i. The Bible plan
(BA.1–BA.8) is DONE. The themes plan: TV.1, TV.2, TS.1, TS.2 DONE. Next is
**TS.3** — shared ink bounds and effect-aware fit/measurement: layout and
`text.measure` grow an ink-bounds result expanded for stroke and shadow,
`fitMode`/`minFontSize` finally drive the shrink pass (fixed = wrap-only),
and style joins the layout cache keys. Then TR.1–TR.3 (transitions; TR.1
claims service format 9).

## How to pick up

1. `PLAN.md` is the contract: ledger rows carry acceptance evidence, the
   themes runbook paragraphs give each leaf's boundary, "Format allocation"
   records the current 8 / 4 / 0.15 numbers (TS.2 needed no bump — it
   advertised the capability TS.1 reserved).
   `docs/reviews/theme-editor-2026-11-03.md` is the design authority.
2. Build `cmake --build --preset desktop --parallel 2` (build dir `build/`);
   test `ctest --preset desktop -j1` (~6.5 min). Headless: `--preset
   headless` (`build-headless/`, 35 tests). Individual binaries:
   `./build/tests/<name>`; `live_fade_tests` takes the worker binary as
   argv[1] (`./build/src/worker/openshow-worker`).
3. TS.3 lives where TS.2 just was — `src/worker/render/SceneText.cpp`
   (`layout()` already carries the effect fields on `LaidOutItem`; the
   shrink loop and `OutputSession::measureText` are the untouched pieces).
   The review's TS.3 contract: ink bounds relative to text origin, glyph
   bounds from the same laid-out runs, stroke expansion half-width, shadow
   expansion the offset-shadow's union with fill (Skia's blur support is
   finite — do not report infinite tails), fitMode "fixed" never enlarges
   or shrinks, and `text.measure` answers with the styled numbers so the
   Bible importer and a styled slide agree.
4. Measurement lore for anyone touching the fade or effects painter: the
   blur sigma is device-space (the painter rescales), stroke width is
   CTM-scaled by Skia, and the fork's shadow pass fires `drawTextShadow`
   once per run BEFORE the blobs — one painter, one fold, no second pass
   needed to suppress duplicates.
5. Boost 1.90 spells doubles in exponent form; manifest assertions match
   style keys by NAME. JSON fixtures use decimal integers. Never hold a
   `const Theme*` across a call that can grow the theme vector.

## Recent commits

- TS.2 — faces/stroke/shadow through public Skia, fade-aware painter,
  measured device-space blur rescale, `scene.text-style.v1` advertised,
  raster + GPU-live fixtures.
- TS.1 (`c5acad7`) — shared text-style contract, service 8, scene 4,
  protocol minor 15, capability gate, controller surfaces.
- TV.2 (`7331a10`) — full-workspace Themes layout, view thumbnails.
- TV.1 (`0f1cf84`) — theme sample views through the preview transport.
