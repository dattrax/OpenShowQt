# OpenShowQt progress and handoff

Updated: 2026-11-03 (Europe/London).

## Current checkpoint

**TS.1 complete (2026-11-03): the text-style contract exists in domain,
storage, scene, protocol and the controller — and the frontend refuses to
describe styling to a worker that cannot draw it.**

The Qt-free domain owns the contract: shared `TextStroke`/`TextShadow`
records and the whole-set `ThemeTextStyle` patch struct, six optional fields
on `Theme` and `SlideCustomization` (`fontWeight` 100–900 default 400,
`fontStyle` normal/italic, `fitMode` shrink/fixed, `minFontSize` 1–4096
default 12, and the two atomic effect records — `enabled:false` is an
explicit refusal, absence is inheritance), and `setThemeTextStyle` replaces
the whole set in `setThemeContexts` manner. The resolver walks
customization → first theme CARRIERS → the contract's concrete defaults, so
an untouched chain resolves to exactly the pixels renderers have always
drawn; `ResolvedAppearance` carries concrete values (effects included) and
contexts say nothing of style until TV.3. One validator family
(`validStroke`/`validShadow`/`validThemeTextStyle`) serves verb, customization
door and loader, so what loads is what a verb would accept.

Service format **8** (`kServiceFormatTextStyle`; era constants are pinned to
the introducing format, never the moving `kServiceFormat`): theme and
customization style keys are written only when set, a hand-bumped pre-8 file
carrying one is refused naming the era, and after shape-reading the codec
re-vets with the domain sentence ("carries text style the renderer cannot
honour: …"). Effect JSON records are closed-member, whole-record-or-nothing;
a stray member names itself.

Scene format **4**: every text item always carries all six style keys
(never defaulted by absence); legacy scene 3 READS onto the concrete
defaults while scenes 1–2 still refuse, and style keys in a scene-3 payload
or on an image item are refused by name. Effect colours cross packed and
compile normalises to "#rrggbb" so a compiled scene equals its own
round-trip. Protocol minor **15** defines `scene.text-style.v1` but
deliberately does NOT advertise it — TS.2 advertises when the renderer
actually draws the fields (protocol_tests pins the absence as a tripwire).
`WorkerController` scans the hello for the capability and gates every scene
send through `sceneIsSendable`: a scene whose text deviates from the
contract defaults is refused naming the capability, with not one byte sent
and the session intact.

Controller surfaces: `setThemeTextStyle(themeId, map)` and the
`customizeCurrentSlide` style fields share one strict `styleFromMap` (unknown
keys refused by name, string weights refused, half effect records refused —
an effect travels whole); `revertCurrentSlideField` speaks the style names;
`slideAppearance()` rows carry value+customized for all six; `DocumentViews`
theme rows carry `...Set` flags with effects as maps; `createThemeFromCurrent`
snapshots the chain's concrete style. **Bug fixed on the way**: `duplicateTheme`
read its source theme through a `const Theme*` invalidated by `createTheme`'s
vector reallocation — latent UB that the new six-field carry made loud; the
source is copied up front, and duplication now carries placement, contexts
AND the whole style block.

Gates: desktop `ctest -j1` **64/64** (372 s), headless build + `ctest -j1`
**35/35** (67 s), `git diff --check` clean. New scenarios: `domain_tests`
(`exerciseTextStyle`: verb bands each naming their own, whole-set semantics
incl. the empty set silencing, resolver first-carrier and explicit-off,
customized refusal over themed effect; `exerciseTextStyleStorage`:
byte-identical styled round-trip, silence writes not one new key, era
refusals for themes and nested customizations via `themed()`/`slideWith()`
fixtures — checked by key NAME because boost 1.90 serializes doubles as
`2.45E1`), `scene_tests` (future tripwire, legacy-3 equality onto defaults,
band/whole-record/style-on-image refusals, widest-legal and disabled
round-trips), `protocol_tests` (capability-absence tripwire),
`document_controller_tests` (`exerciseTextStyleSurfaces`: verb strictness,
one-edit undo/redo, customize/revert badges, duplication carry, from-slide
snapshot, disk reload; manifest pin moved to 8), `worker_controller_tests`
(`scenarioTextStyleGate` with a "text-style" fake peer: gate closed refuses
naming the capability and sends nothing, defaults-only scenes travel, the
claiming peer receives the styled prepare, re-handshake closes the gate).

Retained evidence (TV.2, 2026-11-03): the Themes page reclaims the whole
workspace (`themeMode` hides — never resizes — docks and bin; bounded 220/
300/centre columns; inspector ScrollView reaches at 1000×680), the nav
footer shows live Songs/Bible view thumbnails through the existing preview
transport, and leaving the page restores the arranged workspace, cancels
gestures and returns the keyboard. TV.1 (2026-11-03): `ThemeSample` fixture,
theme view rows, and the sample-based inspector previews through the same
transport. Both gates were green for their own commits (`0f1cf84`,
`7331a10`).

## Where the work stands

Phases 0–5 complete. Phase 6 complete through 6.26i. The Bible plan
(BA.1–BA.8) is DONE. The themes plan is underway: TV.1, TV.2 and TS.1 DONE;
next is **TS.2** — render font faces, stroke and shadow through public Skia
APIs, then advertise `scene.text-style.v1` (updating the protocol_tests
tripwire consciously). After TS.2: TS.3 (shared ink bounds, effect-aware
fit/measurement, style cache keys), then TR.1–TR.3 (transitions; TR.1 claims
service format 9).

## How to pick up

1. `PLAN.md` is the contract: phase ledger rows carry acceptance evidence,
   the themes section's runbook paragraphs give each leaf's boundary, and
   "Format allocation" records the current 8 / 4 / 0.15 numbers.
   `docs/reviews/theme-editor-2026-11-03.md` is the design authority for
   TS.2/TS.3/TR contracts.
2. Build `cmake --build --preset desktop --parallel 2` (build dir is
   `build/`); test `ctest --preset desktop -j1` (~6 min; the multi-main
   UI tests print nothing on pass). Headless: `--preset headless`
   (`build-headless/`, 35 tests). Individual binaries: `./build/tests/<name>`.
3. TS.2 lives in `src/render` (SkParagraph text path) — the contract values
   arrive already resolved on every scene-4 text item; the renderer must
   draw weight/slant through the font face, stroke BEHIND the fill, and
   shadow per the record, all scaled by the fade alpha. Disabled effects
   must equal legacy pixels. After drawing lands, advertise the capability
   in `protocol.h`'s hello list and update the pinned tripwire in
   `protocol_tests`.
4. Notes for whoever edits `duplicateTheme` or adjacent controller code:
   never hold a `const Theme*` across a call that can grow the theme
   vector — copy first (the TS.1 commit fixed exactly this).
5. Boost 1.90 spells doubles in exponent form; manifest assertions in tests
   match style keys by NAME and pin values through round-trips, never
   digit-exact strings. JSON fixtures must use decimal integers.

## Recent commits

- TS.1 — shared typography/effects/fit records, service format 8, scene
  format 4, protocol minor 15, capability gate, controller surfaces,
  duplicateTheme dangling-pointer fix.
- TV.2 (`7331a10`) — full-workspace Themes layout, view thumbnails,
  keyboard handback.
- TV.1 (`0f1cf84`) — theme sample views through the preview transport.
