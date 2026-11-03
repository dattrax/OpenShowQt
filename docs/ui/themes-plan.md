# Themes: editor, apply and restore

Follow-up review (2026-11-03): [theme views, editor space, styling and transitions](../reviews/theme-editor-2026-11-03.md).
TH and TF below describe delivered behaviour. New TV/TS/TR work is TODO in
[PLAN.md](../../PLAN.md); it proposes Songs/Bible navigation, a full-workspace
editor and richer versioned view overrides. TF's typography-only context rule
remains the current format-7 contract until that follow-up is implemented.

Added 2026-11-01; decisions confirmed with the operator the same day.
Scope: service appearance themes across the domain, storage, compilation and
operator window. Reference:
[Themes in ProPresenter](https://support.renewedvision.com/hc/en-us/articles/11910559859603-Themes-in-ProPresenter).

## Goals

1. A Theme Editor opened from a submenu of the main display (More ⋯ →
   Theme Editor), like ProPresenter's `More… → Theme Editor`.
2. "Apply the theme" into songs — per show, per selected slides, or as the
   service's one **current theme**.
3. A quick way to **remove customizations** so a slide follows its theme again
   (single slide, or bulk per show), as one undo step.
4. Resolution priority, fixed in the file and the compiler:
   **per-slide customization → current theme → slide theme → show default
   theme → built-in default.**

## Model decisions

- **One concept.** Today's `Style` already carries exactly the appearance
  fields a theme needs (text colour, background colour and gradient, font
  family and size, alignment, line spacing). It is promoted and renamed to
  **Theme** end to end: `documents.h`, `storage.cpp` (`styles` → `themes`,
  `slide.styleId` → `slide.themeId`, `show.defaultStyleId` →
  `show.defaultThemeId`), controller/QML. The alpha file set is ours; the
  loader migrates format 5 to 6 and strict version rollback is not promised
  (same recorded scope as R8). "Style" remains only in historical task rows.
- **Appearance theme, not template.** A theme is an appearance, not a set of
  template slides with objects, builds and shapes. The storage shape leaves
  room for per-theme `templateSlides` later, and nothing reads it yet;
  templates join PLAN.md's deferred list. This keeps the item model, compiler
  and worker untouched.
- **Current theme** is a service-level field, `currentThemeId`, set in the
  Theme Editor or the More ⋯ theme list. It wins over `slide.themeId` and
  `show.defaultThemeId` for every property it defines, and it is non-
  destructive: slide and show assignments survive and reappear when the
  current theme is cleared.
- **Customization** is a sparse per-slide override struct
  (`Slide.customizations`): only the fields the operator tweaked are present,
  and resolution falls through per property — a slide that customized only
  the background colour still takes the current theme's text colour and font.
  Editing appearance fields on the Edit page writes customizations and never
  mutates a shared theme; theme properties are edited only in the Theme
  Editor. This also removes today's footgun where editing a slide's style
  silently restyled every slide sharing it.
- **Content is not theme.** Words, item stack, geometry, the background
  *image* asset, groups and notes stay authored content. Theme covers colours,
  gradient and typography only.

## Priority audit

| Layer | Stored | Wins over |
|---|---|---|
| Customization | `slide.customizations` (sparse) | everything below |
| Current theme | `currentThemeId` (one per service) | `slide.themeId`, `show.defaultThemeId`, built-in |
| Slide theme | `slide.themeId` | `show.defaultThemeId`, built-in |
| Show theme | `show.defaultThemeId` | built-in |
| Built-in default | `scene.cpp` (white on black, renderer font) | — |

Themes stay as sparse as today's styles (`fontSize` 0 = renderer default), so
the chain falls through wherever a layer defines nothing.

## Storage — format 6

```json
{
  "format": 6,
  "currentThemeId": "theme1",
  "themes": [
    { "id": "theme1", "name": "Bright", "textColor": "#ffd23f",
      "backgroundColor": "#101828", "backgroundGradient": "",
      "fontFamily": "DejaVu Serif", "fontSize": 64,
      "align": "left", "lineSpacing": 1.6 }
  ],
  "shows": [
    { "id": "show1", "name": "Capture Hymn", "defaultThemeId": "theme1",
      "slides": [
        { "id": "slide1", "themeId": "",
          "customizations": { "backgroundColor": "#000000" },
          "items": [] }
      ] }
  ]
}
```

The reader accepts format ≤ 5 and rewrites the old names onto the new model
(`styles` → `themes`, `styleId` → `themeId`, `defaultStyleId` →
`defaultThemeId`, empty `currentThemeId`, no customizations), under the
existing legacy chain; a newer format is still refused. The writer always
emits format 6. Autosave, staged durable saves and asset handling are
unchanged.

## Compilation and worker

The domain exposes one resolution accessor — `appearanceForSlide(showId,
slideId)` walking the full chain — replacing the three `styleForSlide` call
sites (`scene/scene.cpp`, `ui/DocumentController.cpp`,
`ui/DocumentViews.cpp`). Compiled scenes keep carrying fully resolved styles,
so the scene wire, worker, preview streams and acknowledged revision flow are
untouched. A theme change re-compiles affected scenes exactly like any other
document edit: an on-air slide follows the edit through the existing
re-prepare/re-present path and its LIVE marker waits for the acknowledgement,
inside the existing 100 ms p95 budget for preloaded changes.

## Operator UI

- **More ⋯ menu** (header, beside the page tabs): the theme list with the
  current one checked — clicking sets or clears the current theme — and
  "Theme Editor…" below a separator. ProPresenter parity for the entry point;
  Settings stays a mode at the far edge.
- **Theme Editor** (a mode like Settings): theme list left (New, Duplicate,
  inline Rename, Delete — references cleared on delete, one undo step),
  inspector right (every theme property), centre a large live preview of a
  built-in sample cue rendered in the selected theme through the existing
  worker-rendered selected-scene preview (6.16a lane). The sample is a
  fixture, never a document, so browsing themes touches no output. A "Use as
  current theme" toggle sits in the editor header.
- **Apply surfaces**: a theme selector for the current show in the slide
  workspace header; cue-grid context menu (Apply theme ▸, Remove
  customization); sidebar show-row context menu (Apply theme to show ▸,
  Remove all customizations in show). Bulk operations land as one undo step.
  Delivered shape (Qt 6.12): the header selector is a plain button over a
  menu — a Material ComboBox carries a writable text field, and the Show
  screen's audit permits none — and both context menus are FLAT (theme rows
  directly, restore below a separator): a nested submenu's Repeater delegates
  never instantiate in this Qt version, so the "▸" spelling would be a menu
  that cannot open. The header names the VIEWED show's default — browsing a
  library presentation sets that presentation's own default, never the one
  under preparation.
- **Edit inspector**: fields show the resolved appearance; customized fields
  carry a badge with a per-field revert, and a "Remove customizations" action
  for the whole slide. Field edits write customizations (see Model). Delivered
  detail: `editingFinished` also fires on a bare focus-out, so committing a
  value the chain already resolves to is treated as no edit — leaving a field
  untouched cannot customize it.

Boundaries that hold throughout: browsing or previewing a theme never changes
the audience output; every mutation is an acknowledged document edit through
the single history owner; with the worker disconnected the documents stay
editable and the live preview is honestly blank, as elsewhere.

## Amendment (2026-11-01): themes carry placement

The operator found the shipped milestone half an answer: a theme recoloured
every slide wearing it but left the words wherever each slide's own box
happened to sit. Placement is part of what a theme *is* — "the same look"
means the same place too. Decisions taken with the operator:

- **Theme placement** is one rectangle — the primary lyric box — in the
  1920×1080 authored canvas. Absent (the default, and every theme written
  before this amendment) means the theme has no placement opinion and every
  slide draws its own authored box exactly as today, so no existing document
  moves on open or on migration.
- **Chain**: placement resolves per the same walk as appearance — slide
  placement customization first, then the current theme, then the slide's
  theme, then the show default. With no layer speaking, the slide's own
  authored rectangle is the answer (which for untouched slides is the
  built-in lyric box). The theme rectangle drives the slide's FIRST TEXT
  ITEM only; additional boxes and every image item keep their authored
  positions — a deliberate stacked slide does not collapse.
- **Theme wins over the authored rectangle**: applying a themed placement
  lines up every wearing slide immediately (this is what "text in the same
  place for each slide" means). A slide that drags its box or types
  coordinates records a placement CUSTOMIZATION (one unit: x, y, w, h) and
  keeps its own spot, with the usual revert that hands the box back to the
  theme. Geometry edits to a box the theme does not drive keep writing the
  authored rectangle exactly as before.
- **Theme Editor** gets the slide editor's placement idiom: the sample
  preview wears the theme's rectangle as a draggable, corner-resizable box
  (same chrome, tolerance and floors as the Edit canvas) committing one
  placement per gesture, plus X/Y/W/H fields and a "no placement" clear in
  the inspector. Browsing a theme with placement moves the sample's box,
  still with no word of it reaching the worker output.
- **Storage** stays format 6: `placement` is an optional member of a theme
  entry and of a slide's `customizations` (a `{x,y,width,height}` object,
  vetted like an authored box — finite, positive extent). Absent is
  meaningful: no opinion, nothing moves.

Leaves: **TH.7** (domain, storage, compile: the rectangle through the chain)
and **TH.8** (the surfaces: Theme Editor canvas, Edit routing, badge,
captures, documentation).

Shipped 2026-11-01. TH.7 landed the chain matrix and the format-6 round-trip
with its refusals; TH.8 landed the surfaces and found two things the fixture
pinned rather than worked around: the Edit page's bottom band sits under the
Flickable's viewport edge at capture height, so the placement controls are
only clickable after the same flick an operator makes; and a QML handler
fired from inside a C++ `documentsChanged` emit reads the stale `row`
binding, so the Theme Editor's inspector re-reads through `rowFor` and
re-syncs when the page is shown after sleeping through outside edits — a
re-shown page that still displays the numbers from before someone else's
write was the bug the themed `.themes.png` capture check caught. The
capture pair now proves the story end to end: House places 240/620/1440/320,
the Edit capture shows the canvas chrome drawn where the audience sees the
box under THEME PLACES, and the Themes capture shows the same rectangle
solid on the sample with the fields holding its numbers.

## Amendment 2026-11-02 — the Default floor and universal placement edits (TH.9)

The operator answered two open questions from the amendment's delivery:

- **Every move of the primary lyric box is a customization.** TH.8's routing
  ("a box the chain does not drive keeps editing authored geometry") is
  superseded for the slide's FIRST TEXT item: a drag or typed geometry write
  on the PRIMARY box always records `customizations.placement`, whether or
  not a theme currently speaks. The authored rectangle becomes read-only
  legacy for that row — the compile still uses it when the whole chain is
  silent, but no UI write touches it any more — so a themed slide can be
  nudged without first customizing it into place, and reverting placement
  returns the slide to the theme (or to the authored legacy rect, or to the
  Default floor) rather than to a half-written mixture. Additional text
  boxes and every image item keep writing their authored rectangles exactly
  as before. The Edit page's owner column answers for the primary row in
  three states — THIS SLIDE (a placement customization is worn), THEME
  PLACES (a theme's rectangle is applied), AUTHORED (nothing speaks; the
  legacy rect is what the worker draws) — and `slideBoxes()` carries the
  APPLIED rectangle for that row, so canvas chrome is drawn where the
  audience sees the words even when no theme is in play.
- **The built-in look is an embedded, undeletable Default theme.** A theme
  with the reserved id `default` — outside the `styleN` ordinal sequence, so
  a migrated document can never collide with it — is seeded into every
  `Documents` at construction (where it takes row 0 of the list) and
  re-ensured after every successful service load, so no open document is
  ever without it and it is always the last layer of every chain walk. Its look (`#ffffff` on `#000000`, flat, platform font, size 0,
  centered, default line spacing, no placement) is pixel-identical to the
  pre-amendment built-in rendering. It is editable like any theme (rename,
  recolour, place) but `removeTheme("default")` refuses with "the default
  theme cannot be removed", the Theme Editor's Delete button is disabled
  while it is selected, and it is what the header's clear-current lands on
  as the fallback. A document whose one and only theme is Default cannot be
  emptied further.
- **Restore customizations already covers both.** Per the operator: "Restore
  customizations can just remove any custom edits on that slide. I don't
  think we need to do more than this." Placement is a member of the slide's
  customization set, so the one existing restore removes look AND placement
  in its single undo step; no separate "remove theming" entry was added.

Shipped 2026-11-02. Note for legacy files: a slide whose authored primary
rectangle was dragged before this amendment (the operator's slide 1 carries
an authored y=261) keeps resolving to that authored rect — it is not
silently converted; typing the intended numbers once turns the row into a
customization and everything after that behaves uniformly.

## Task order

TH.1 → TH.2 → TH.3 run in sequence (naming before model before compile);
TH.4 and TH.5 follow TH.2/TH.3 and can interleave; TH.6 closes the milestone.
Each leaf passes its PLAN.md acceptance row before the parent is DONE.


Acceptance follows the plan-wide rules: focused domain tests for the chain,
journey tests against a real worker for pixels, captures for layout. No claim
of physical-display behaviour beyond what the projector check (7.1c) already
gates.
