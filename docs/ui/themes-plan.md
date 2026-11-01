# Themes: editor, apply and restore

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

## Task order

TH.1 → TH.2 → TH.3 run in sequence (naming before model before compile);
TH.4 and TH.5 follow TH.2/TH.3 and can interleave; TH.6 closes the milestone.
Each leaf passes its PLAN.md acceptance row before the parent is DONE.


Acceptance follows the plan-wide rules: focused domain tests for the chain,
journey tests against a real worker for pixels, captures for layout. No claim
of physical-display behaviour beyond what the projector check (7.1c) already
gates.
