# Transparent lyrics and Bible presentations — 2026-11-07

Lyrics and Bible themes style presentation content: text colour, typography,
placement, effects and transitions. A theme cannot supply a background fill or
gradient. Explicit backgrounds on authored slides and media remain separate.

Service format 11 omits theme background settings and refuses those settings in
new files. The loader still accepts formats 1–10, retiring their theme fills
while preserving text styling, placements, contexts, assets and assignments.
Legacy C++ theme-call background arguments remain accepted for compatibility but
are never stored or resolved. The theme inspector hides the two background
fields and its adapter refuses background writes.

Scene format 5 represents an absent backdrop explicitly with
`background.transparent`. Scenes 3/4 remain readable as opaque legacy scenes.
Transparent scenes cannot claim a background image or gradient. The worker's
background draw skips absent backdrops. Off-air Selection/Thumbnail frames clear
to transparent and retain premultiplied RGBA through shared memory and Qt.
Audience frames still composite to opaque output.

Presenting transparent content replaces the old presentation items while keeping
the independently active background, including a video identity. Prepared scenes
stay immutable and transparent for previews. The effective queued composition
participates in the cache's held-asset walk, so evicting a prepared revision does
not lose an inherited background. Explicit Clear Background/Clear All commands
remain explicit opaque clears and do not inherit the previous background.
The frontend mirrors the composed scene for acknowledgement-backed indicators,
clear and restore behavior.

The preview painter shows a small, muted checkerboard behind real alpha pixels.
It is UI chrome, never baked into scene content, worker frames or audience output.
Opaque media cover it; letterboxing stays black. A cached brush avoids repainting
thousands of individual checker rectangles each frame.

Song captions use `Section – Song name` in Show, Edit and import previews.
Consecutive repeats still omit the entire caption text, retaining the section
colour and lyric image. No slide-number or font-size prefix is added.

Regression coverage includes actual alpha-zero background pixels and glyph ink,
a fully transparent blank cue, real-worker thumbnail alpha and checkerboard
painting, caption titles/repeated sections, background retention and explicit
clear, and format-11 storage/legacy migration. Validation results are recorded
in PROGRESS.md after the affected suites complete.


Validation completed: the desktop build succeeds. The 19 affected suites pass
in 168.32 s: domain, scene codec/compiler, scene store, preview producer,
workspace UI, shared controls, UI load, document controller, inspector adapter,
scene edits, worker controller, real document presentation, Library/service UI,
clipboard import page, song service, Bible import page, CPU scene rendering,
preview selection and PowerPoint UI. Final acceptance passes 11/11 in 34.85 s
at 150% scale, additionally covering frame composition, background transport,
transition rendering, production composition and the no-render-layer guard.
Normal and 150% Show/Edit screenshots were reviewed, including the large Edit
canvas. Both real song and Bible preview assertions verify alpha-zero pixels.
`git diff --check` passes.
