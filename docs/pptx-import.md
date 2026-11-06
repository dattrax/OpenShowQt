# PowerPoint deck import (PX)

OpenShowQt imports a `.pptx` as static slides: one PNG per visible page at the
1920×1080 authored canvas, with no animations and no PowerPoint or LibreOffice
runtime. The renderer is [BetterOffice](https://github.com/openooxml/betteroffice),
pinned as the `third_party/betteroffice` submodule and driven by the separate
`tools/pptx-render` process. A malformed deck can only make that process exit
nonzero; it cannot take the operator's session with it.

## Operator flow

- **Drop on the running order** → the deck becomes a schedule-owned
  presentation inserted at the marked position. It is available for this
  service, appears in the running order, and is never a Library row.
- **Drop on the Library's Presentations header** → the deck becomes an ordinary
  Library presentation and is not added to the running order. It is available
  next time the service is opened.
- Both paths are offline, one edit and one undo. The whole import — rendered
  assets, show and running-order row — undoes together.
- Hidden slides are skipped. `.ppt` and `.pps` are refused with "save it as
  .pptx first"; older binary PowerPoint files need a one-time conversion.
- While a deck is rendering, both drop targets are disabled.
  The status line reports the imported page count or the renderer's own
  refusal.

## Storage

The service package is unchanged: `service.json` plus `assets/`. Each rendered
page is an ordinary picture asset named `<deck> - NN.png`, and its slide is
background-only with `backgroundAssetId` pointing at it and `fit` set to
`contain` (so 4:3 and 16:10 letterbox rather than crop).

Bitmaps are **not** base64 or another ASCII encoding inside `service.json`.
PNG is already compressed, encoding adds roughly a third to every file, turns
the editable manifest into a multi-megabyte blob, and defeats the save-time
asset-reuse ledger by rewriting every bitmap on every autosave. Separate files
keep the manifest hand-editable, let one render be replaced or repaired without
touching the model, and confine a corrupt asset to that asset. A single-file
ZIP export of the same package is a later convenience, not the storage format.

Renders are produced under
the application cache (`QStandardPaths::CacheLocation`) under `pptx/<sha256-of-deck>/` and copied into `assets/` by
the next save (or autosave). A repeated import of unchanged bytes reuses the
cached render. A failed render removes its partial cache directory. After a
save the package is self-contained; reopening never needs the renderer.

Deleting a Library presentation removes the show but not shared media:
duplicates and pastes may still reference a render. **More → Remove unused
media** is the explicit sweep; it removes only assets no slide references, in
one undo.

## Renderer and build

```sh
./build/cargo/release/pptx-render --input deck.pptx --out-dir /tmp/deck-slides
```

The sidecar writes zero-padded page PNGs and `manifest.json` with dimensions,
source page numbers, hidden pages, skipped images and font substitutions. The
normal CMake presets build it; `-DOPENSHOW_BUILD_PPTX_RENDER=OFF` disables the
feature. It needs Rust 1.85+ (edition 2024); the tool pins 1.96.0 in
`tools/pptx-render/rust-toolchain.toml` and builds with `--locked`. The first
build fetches crates from crates.io. The BetterOffice submodule is shallow;
`git submodule update --init --recursive` gets it with Skia.

The bundled Latin font pack gives Linux and Windows the same metric-compatible
faces (Carlito, Caladea, Liberation). Families the pack cannot match are listed
in the manifest and in the operator status line; install the font or accept the
substitution. CJK and other scripts need the optional BetterOffice font packs
in a later build.

## Known limits

- Charts and SmartArt are the weakest fidelity area in BetterOffice. Standard
  text, shapes, tables, pictures and backgrounds are strong; a chart-heavy deck
  should be checked visually. An optional LibreOffice fallback remains a
  separate, deferred decision and is not a dependency.
- Animations, transitions inside the deck and embedded video are not imported.
  The static first frame is what becomes the slide.
- Imported slides are pictures, not editable text or shapes. Re-import the
  deck to change it.
- The source `.pptx` is not stored in the package; the rendered PNGs are the
  durable artifact. Keep the deck if you may want a 4K re-render later.

## Tests

- `tools/pptx-render/tests/render.rs` — scale math, hidden slides, manifest,
  determinism, corrupt input, Unicode paths and the committed operator deck
  (`aug-8.pptx`, six pages).
- `document_controller_tests` — both destinations, one undo, cache reuse,
  missing renderer, bad manifest, renderer failure, save/reopen offline,
  deletion and the unused-media sweep.
- `pptx_ui_tests` — real URL `QDropEvent`s over the service and Library
  targets and the operator deck through both doors; redundant menu entries and
  their file picker are absent.
- `document_present_tests` — all six operator-deck slides on a live worker,
  then the saved package reopened with the renderer absent and presented again.
- `.github/workflows/pptx-render-windows.yml` — MSVC build and the same
  contract on Windows.
