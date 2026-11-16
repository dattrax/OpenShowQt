# Text layout and editor consistency — 2026-11-16

Implementation brief for the **TL** group in `PLAN.md`. This is planned work;
no new layout feature or runtime verification is claimed by this document.

## Agreed operator behaviour

- Theme Editor and Edit use the same vertical-alignment controls and **All caps**
  checkbox. Theme Editor edits theme defaults or its selected Bible view; Edit
  writes per-slide overrides. All text boxes on that slide share the resolved
  setting, as they share typography today. Individual-box overrides are deferred.
- A change updates the off-air preview immediately through the existing worker
  request path. Audience content changes only on the next explicit Go, including
  when editing the currently live slide or a theme it wears. No automatic
  `scene.present`, live output switch or new playback command is introduced.
- Defaults are vertical **Centre** and **All caps off**. An old service retains
  its pixels, source text, item IDs and authored slide boundaries.
- All caps is reversible display formatting. Stored lyric lines, Library words,
  clipboard content, titles, notes and attribution metadata keep their original
  case. Turning the checkbox off displays the original case again.

## Current implementation and consistency findings

`AppearanceInspector` already composes `TextStyleControls` over one
`InspectorAdapter` for Theme Editor's Songs/Bible views and the Edit page.
Extend those components rather than introducing page-specific layout controls.
The adapter owns routing, own/inherited provenance and reset behaviour; the
domain resolves the appearance chain and the worker owns final layout.

The worker currently places every text block using
`SceneTextEngine::paragraphTop()`, which returns
`boxY + max(0, (boxHeight - paragraphHeight) / 2)`. Vertical alignment is
therefore fixed at centre, not a property that either page can edit.

Edit's `themePanel.revertableNames` independently enumerates customization
fields and omits `fontUnderline`. An underline-only customization therefore
does not enable Remove customizations. Retire that separate enumeration: expose
`DocumentController.hasCurrentSlideCustomizations`, derived from the selected
slide's domain customization record, with document/navigation notifications.
Use it for the button, including underline, the two new fields, explicit false
overrides, effects and placement. Individual reset chips still use the adapter.

The Bible inspector currently hides the style block because its context cannot
store weight/effects yet. The new layout controls must be available there through
explicit field capability, independent of `showStyleBlock`; do not enable
unimplemented effects or mark TV.3 complete as a side effect of this work.

## Domain, persistence and wire contract

Use canonical names `verticalAlign` and `allCaps` across document adapters,
resolved scenes and measurement requests:

- `verticalAlign`: exactly `"top"`, `"center"` or `"bottom"`.
- `allCaps`: a strict boolean. `false` is an explicit override, distinct from
  absence/inheritance.

Both are optional in theme style records, named text contexts and slide
customizations, and concrete in resolved appearance and compiled text items.
Follow the existing customization → selected context/theme chain → built-in
resolution, preserving the first-context-wholesale rule. A selected context's
missing field inherits the relevant theme answer; resetting it removes the
override. Built-in values are `"center"` / `false`.

Include the fields in validation, equality, empty/has/clear-field helpers,
theme creation/duplication, whole-style replacement, context merge, undo/redo,
controller read models, inspector provenance and serialized content. Changing
one field must preserve the other fields, box placement and transitions.

At this planning checkpoint the code uses service **13**, scene **7** and
protocol **0.19**. Reserve service **14**, scene **8**, protocol **0.20**, with
capabilities `scene.text-layout.v1` and `text.measure.v3`. Recheck allocation
before implementation if another task changes these versions. The older TV/TS
runbook's 9/4/0.18 paragraph is historical, not the current allocation.

- Service 14 persists sparse layout fields. Readers of services 1–13 supply
  legacy centre/off defaults without changing content or IDs. New keys in an
  old-format record, unknown enum values, non-boolean caps and malformed records
  refuse atomically, as existing strict readers do.
- Scene 8 carries concrete layout fields on text items. Scene 3–7 readers keep
  centre/off behaviour. Image items do not acquire ignored text-style keys.
- Existing `text.measure` accepts the two optional fields under v3; omitted
  fields retain existing behaviour. Preserve v1/v2 requests and response shapes.
  `verticalAlign` changes positioned bounds when a box height is supplied,
  not the intrinsic text height returned to a paginator.
- Gate non-default layout toward older workers locally, with an actionable
  refusal. For centre/off content only, retain a scene-7/default measurement
  payload when the peer lacks the new capabilities; never send fields it would
  silently ignore. Clear/restore scenes use the same compatibility rule.

## Worker layout and text transformation

Apply uppercase once in the shared worker paragraph-building path, before
SkParagraph shapes, wraps, fits or measures the text. Send original UTF-8 lines
plus the resolved flag through the API; do not rewrite lines in QML, mutate
stored documents or implement an ASCII-only transform at individual call sites.

Use the existing public Unicode service's locale-explicit uppercase operation
with ICU's root locale (`""`), so output does not depend on the machine's user
locale. This is full Unicode string uppercasing, not title case, case folding
or a byte-by-byte operation. It may expand a character into several characters;
preserve authored line breaks and empty lines and apply existing text/resource
limits to the transformed result, returning a named failure on overflow.
Language-specific casing preferences are outside this task.

Rendering and `text.measure` must enter that same transformation/layout path.
Keep source words alongside the presentation flag; do not uppercase twice in
an importer and again in the renderer. Extend layout/measurement keys to include
the new resolved settings wherever caches use values rather than scene identity.
The transformed text and its actual font determine wrapping and shrink-to-fit.

After fitting, let `free = max(0, boxHeight - paragraphHeight)`. The paragraph's
top is `boxY` for top, `boxY + free / 2` for centre, and `boxY + free` for
bottom. Align the paragraph's layout block, not its shadow or stroke extent;
effects retain their existing overhang semantics. An overflowing fixed-size
paragraph stays top-anchored for all three options, preserving the old clamped
centre behaviour. Drawing, ink bounds and fit diagnostics use the same origin.

Song/Bible import plans measure with the same resolved settings, include them
in fingerprints/cache keys, and retire stale measurements/previews when a flag,
view or target changes. Add must commit only the current measured plan.
Preserve the original-case words and labels in generated slides. Existing
committed slides are restyled in place; do not automatically repaginate,
replace the Library content or change authored slide boundaries on a toggle.

## Shared controls and interaction

- Add a Vertical row beside the existing horizontal alignment row in the shared
  text controls. It has exactly three segmented icon choices: Top, Centre and
  Bottom. Draw simple in-repository QML/vector paragraph marks positioned against
  top/middle/bottom guides; reuse the existing segment styling and active accent.
  Do not depend on a platform font's icon glyphs or copy PowerPoint artwork.
- Give every icon a tooltip and accessible name; support focus and keyboard
  activation as well as pointer clicks. Keep horizontal and vertical labels
  unambiguous and use the same order/spacing on Songs, Bible and Edit.
- Add one **All caps** checkbox in that shared layout section. It shows the
  resolved boolean, with the same own/inherited indication and reset chip as
  the other style fields. Clicking an inherited checked value off authors
  `false`; reset removes the override and can make it checked again.
- One committed gesture is one undo step. A rejected edit restores the displayed
  value; programmatic sync, focus changes and page navigation create no edits.
  Readiness follows the current worker preview; late replies cannot put the
  prior casing/alignment under the new controls. No QML substitute text layout.

## Acceptance and implementation order

Use the TL ledger in `PLAN.md`; target 0.5–2 engineer-days per leaf and split
further if needed. Implement domain/storage → wire/adapters → worker → import
flow → shared controls/consistency → integrated acceptance. Do not advertise
worker capability before its rendering and measurement behaviour exists.

Required evidence:

- Domain/storage/scene: defaults and legacy migration, precedence, context
  inheritance, explicit false, copy/rename, individual/whole reset, one undo,
  save/reopen and strict malformed-input refusal without partial changes.
- Worker raster/measurement: short/multiline text at each vertical position,
  fixed overflow and shrink fit, horizontal alignment combinations, effects,
  multiple boxes, alpha previews and output scaling. Default pixels remain
  unchanged. Bounds match the same origin used by paint.
- Caps: ASCII, accented Latin, Greek/Cyrillic, a length-expanding mapping,
  combining marks and uncased/emoji content; original mixed case survives
  toggle/undo/save/reopen. Use pinned fonts where a pixel assertion needs them.
- Flow/IPC: uppercase-induced wrapping reaches both pagination and pixels;
  original words are conserved, caches invalidate, delayed old plans cannot
  enable Add, and older peers are either sent safe defaults or refused before
  non-default content is sent.
- Shared UI: Songs, Bible and Edit at 1000/1380/1680 widths; all three icons and
  the checkbox are reachable by mouse/keyboard, show matching inheritance/reset
  treatment and follow page/view selection. Underline-only and all-caps-false
  customizations enable Remove customizations.
- Journey: change an on-air cue's formatting, observe only the off-air preview
  change, then Go and verify acknowledged audience pixels with unchanged media
  identity continuing. Complete undo/reset and save/reopen, including Library
  song and scripture drafts, without editing their source words.

Run focused regressions first, then desktop/headless presets and source guards.
Record skips and physical acceptance separately; planning does not close TV/TA,
ND or M7 gates. Update the control-set catalogue/architecture and archive detailed
completion evidence rather than growing the PLAN ledger into another run log.
