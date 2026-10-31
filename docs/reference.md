# FreeShow 1.6.7 operator reference

The FreeShow measurements below remain historical behaviour evidence. As of
2026-10-31, the main workspace layout/palette follows the user-supplied
ProPresenter image in `third_party/cache/propresenter-visuals/`; Show presents
slides, while Edit owns text/slide authoring.

The accompanying [Renewed Vision UI guide](https://support.renewedvision.com/hc/en-us/articles/360041345954-Understanding-The-ProPresenter-User-Interface)
is the structural reference for further workspace work:

- Fixed toolbar for search, modes and media visibility.
- Left sidebar: library/playlist outline above the selected collection's contents.
- Centre: presentation or playlist slides, with view and thumbnail-size controls.
- Bottom: media bin, with collections and media cues.
- Right: audience preview, adjacent layer clears, transport below, then Show Controls.

The current workspace is an initial approximation. Its duplicated bottom show
library should become the media bin; the left outline/detail split, playlist-wide
slide view and right-hand control arrangement need further work. These are open
layout differences, not completed acceptance checks. Existing deferred features
remain deferred, and the user's Edit-only authoring requirement takes precedence
over ProPresenter's inline authoring controls.

Derived from the source clone, not from the running application. FreeShow 1.6.7,
commit `436d4b634878ba723da1d1373637384ec700ce16` at `/home/jim/FreeShow`.
Electron main process plus a **Svelte** renderer (547 `.svelte`, 466 `.ts`).

Citations are `path:line` relative to that clone. Shorthands: `FS` =
`src/frontend`, `EM` = `src/electron`, `T` = `src/types`. Rows and claims marked
`RT` were additionally measured on a running 1.6.7 Linux AppImage over the
Electron DevTools protocol; section 11 records what that inspection settled and
what it could not, and section 12 how to reproduce it.

## 1. Terminology

FreeShow's words are not always ours.

| FreeShow | Meaning | OpenShowQt |
|---|---|---|
| project | a **playlist** of shows, media and sections (`T/Projects.ts:3-46`) | playlist |
| show | a deck of slides, stored as one `.show` file | show |
| layout | an alternate slide set inside a show (`T/Show.ts:320-327`) | layout (deferred) |
| group | chorus/verse label on a slide (`T/Show.ts:196-206`) | group |
| style | reusable presentation: layers, background, transition, aspect (`T/Settings.ts:13-45`) | style |
| output | a user-defined output, each its own window (`T/Output.ts:4-39`) | display output |
| `out` | the per-output program buffer actually being shown (`T/Output.ts:72-80`) | render scene |
| drawer | bottom library strip with tabs | library drawer |
| page | top-level mode: show, edit, stage, draw, settings | workspace |

## 2. Application shell

| Element | Facts | Source |
|---|---|---|
| Root order | `Top` → row(`left`, `center`, `right`) → optional project timeline → `Drawer` → `Tipbar` | `FS/MainLayout.svelte:35-131` |
| Left panel | resizable `leftPanel`; per page: show→`Projects`, edit→`Navigation`, stage→`StageLayouts`, draw→`DrawTabs`, settings→`SettingsTabs` | `FS/MainLayout.svelte:40-56` |
| Center | `flex:1`, background `--primary-darker`; show→`Show`, edit→Editor, settings→Settings | `FS/MainLayout.svelte:58-74` |
| Right panel | resizable `rightPanel`, always `Preview`, then per-page tools (`ShowTools` on the show page) | `FS/MainLayout.svelte:76-107` |
| Panels nest in the drawer too | the drawer repeats the same three-column structure with its own widths: `#leftPanelDrawer` (categories), a center list, `#rightPanelDrawer` (item info), each persisted separately | `FS/stores.ts:357` (`RT`) |
| Measured at 1680x1020 | top bar 40; `#leftPanel` 290 (286 content + 4 handle); `.center` 1100; `#rightPanel` 290; `.row` 680; drawer 300 | `RT` |
| Splitter | the panel div is itself the drag target: a mousedown within `handleWidth` of its inner edge starts a resize (`handleWidth` 4, or 6 once the panel is collapsed to 8 or less); default width 290, max `290 x 2.2` = 638, snap: below `290 x 0.69 / 2` = 100.05 it closes to the handle width, 100.05-200.1 floors at 200.1, 270-310 snaps to 290, above 638 clamps | `FS/components/system/Resizeable.svelte:18-22,41-58` (verified by dragging in the running app) |
| Collapsed panel | `class="panel bar_left zero"`, `overflow: hidden`, content clipped to 0, a white `arrow_right` icon shown in the strip; clicking the strip restores 290 | `FS/components/system/Resizeable.svelte:73-99` (`RT`) |
| Right panel goes two-column | above 522px the inner container gains class `right row` and computes `flex-direction: row-reverse`, so the tools column sits inside and the preview column stays nearest the screen edge; each column keeps its own width (measured 293 of a 590 panel) | `FS/MainLayout.svelte:77,171-182` (`RT`) |
| Top bar | height 40, drag region, title at left (`width: var(--navigation-width)`), page buttons centred, settings and output buttons right | `FS/components/main/Top.svelte:94-158` |
| Unsaved marker | 5x40 strip at the left edge of the top bar, `rgba(255,0,0,.25)` | `FS/components/main/Top.svelte:99-101,205-211` (`RT`) |
| Window | native frame on Linux; min 400x400; default 800x600 | `EM/index.ts:202,211-212,232` |
| Focus mode | hides `Top` and `ShowTools` | `FS/MainLayout.svelte:36-38,80` |
| Layout persistence | `resized` widths, drawer height, enabled tabs, slide options all live in the settings store, not the show file | `FS/stores.ts:344-357`, `FS/utils/save.ts:171-186`, `EM/data/defaults.ts:44-49` |
| Load quirk | persisted panel width is applied only after a 2s delay; panels at or below 4px reset to 290 at startup | `FS/components/system/Resizeable.svelte:27-34` |

## 3. Default theme and typography

Values from `public/global.css`; at runtime the theme pushes `--{key}` overrides
onto `:root` (`FS/utils/updateSettings.ts:256-259`).

| Token | Value | Token | Value |
|---|---|---|---|
| `--primary` | `#242832` | `--primary-lighter` | `#2f3542` |
| `--primary-darker` | `#191923` | `--primary-darkest` | `#12121c` |
| `--text` | `#f0f0ff` | `--textInvert` | `#131313` |
| `--secondary` (accent) | `#f0008c` | `--secondary-opacity` | `rgba(240,0,140,.5)` |
| `--accent` | `#90caf9` | connected / disconnected | `#27a827` / `#a82727` |
| `--hover` | `rgba(255,255,255,.05)` | `--focus` | `rgba(255,255,255,.1)` |
| `--navigation-width` | 290px | font stack | system UI stack (`global.css:53`) |
| base size | `1em`, headings `--secondary` weight 600 | scrollbars | 8px, thumb `rgba(255,255,255,.3)` |

Source: `public/global.css:31-56,85-86,97-110,125-138`,
`FS/components/settings/tabs/defaultThemes.ts:3-60`.

## 4. Pages and library drawer

| Page | Key | Left | Center | Right |
|---|---|---|---|---|
| Show | `1` | projects and playlist | slide grid, layouts bar | output previews + show tools (Groups, Metadata, Media) |
| Edit | `2` | slide navigation | editor | media, audio, effect, scene and edit tools |
| Stage | `3` | stage layouts | stage layout editor | stage tools |
| Draw | `4` | draw tabs | slide canvas | draw settings |
| Settings | `5` | settings tabs | settings page | context tools |

Sources: `FS/MainLayout.svelte:40-107`, `FS/components/inputs/TopButton.svelte:17-58`,
`FS/values/tabs.ts:3`.

Drawer tabs, in order, with `Ctrl+1…8`: **Shows, Media, Audio, Overlays,
Templates, Scripture, Calendar, Functions** (`FS/values/tabs.ts:5-16`,
`FS/utils/shortcuts.ts:180-188`). Default tab `shows`, height 300, collapsed 40,
`Ctrl+D` toggles when nothing is selected (`FS/stores.ts:63,346`,
`FS/components/drawer/Drawer.svelte:28-30,81-111,184-185`). Verified live: the
drawer goes 300 to 40 and back and the row above it grows 680 to 940 accordingly.
Search is **not** a permanent field: a 100x36 "Search" button sits at the right end
of the tab bar, and `Ctrl+F` swaps it for `input.search.edit.drawer_search`,
290x36 (the right-panel width) with placeholder `Search...`, which takes focus;
`Esc` collapses it back (`display: none`)
(`FS/components/drawer/Drawer.svelte:164-227,287-292`) (`RT`). Shows and Media lists are
virtualised (`FS/components/drawer/pages/Shows.svelte:272`,
`FS/components/drawer/media/Media.svelte:616`).

Settings tabs, in order (`RT`): **General, Outputs, Styles, Audio, Connection,
Files, Profiles, Theme, Smart, Other**. The Outputs tab lists one row per output
with name, `Use style`, the window bounds (1920x1080), **Output screen** and
**Always on top**, plus a `New output` button — that is the whole surface our
display-selection operation has to cover
(`FS/components/settings/tabs/Outputs.svelte`) (`RT`). Quick search (`Ctrl+G`) is a
600px box centred in the window with placeholder `Quick search...`, empty until
typed into (`RT`). The edit page shows `Slide | Chords | Text edit` tabs over the
canvas, a `Textbox | Item` tool stack on the right and slide navigation with a
`New slide` button on the left (`RT`).

## 5. Show page: playlist, grid, controls

FreeShow calls the playlist a **project**. There is no program/preview pair: each
output has one live mini-preview that mirrors the real output window, and
go-commands go to the active outputs
(`FS/components/output/preview/MultiOutputs.svelte:246-344`).

| Area | Behaviour | Source |
|---|---|---|
| Project list | folders plus projects, sortable by name, created, modified; archive collapsed behind "Show archived (n)" | `FS/components/show/ProjectList.svelte:19-62,145-212` |
| Project row | click opens, `Alt`+click opens without selecting first item; inline rename | `FS/components/show/ProjectList.svelte:103-108,168-178` |
| Add | floating **+**: single click opens the add menu, double click creates directly | `FS/components/show/Projects.svelte:545-582` |
| Playlist rows | sections are coloured header rows with optional time and trigger icon; shows are rows between them | `FS/components/show/ProjectContentList.svelte:266-349` |
| Reorder | HTML5 drag within the playlist, and from the drawer into the playlist, rewrites the project's item list at the drop index | `FS/components/helpers/drop.ts:21,30,42-78`, `FS/components/helpers/dropActions.ts:79-135` |
| Slide grid | modes `grid` (default), `list`, `lyrics`, `simple`, `groups`; columns default 4 with a zoom slider; cycle `Ctrl+Shift+V` | `FS/stores.ts:247`, `FS/components/show/Slides.svelte:493,517-529`, `FS/components/slide/Layouts.svelte:188-192` |
| Slide card anatomy | `div.slide.context.#slide` wraps `.hover.overlay` (the hover wash) plus `div#slide[data-item="{index,showId}"][draggable=true].selectElem`, inside it `div#<outputId>.zoomed` then `.slide.hideOverflow` with `aspect-ratio: 16/9` and a `span.zoom` whose CSS `zoom` is cardWidth/1920 (measured 0.13979 for a 268.4px card); the grid is `droparea > ParentBlock > grid > main.active`, `main.active` marking the on-air card | `FS/components/slide/Slide.svelte:277,448` (`RT`) |
| Thumbnail label | a `.label[data-title="<group name>"]` bar under the card showing the 1-based number at the left; the active card's label carries `border-bottom: 2px solid var(--primary-darkest)`, edit selection a 5px text-colour outline | `FS/components/slide/Slide.svelte:546-552`, `FS/components/system/SelectElem.svelte:331,364` (`RT`) |
| Click to go | a plain single click on a slide sends it to the output immediately; `Ctrl`/`Shift` click only selects; `Alt` goes without extras | `FS/components/show/Slides.svelte:96-99,143-144` |
| Hover icon strip | two `.icons` flex columns flank the card (left and right edges, card height); hovering paints `.hover.overlay` with `rgba(255,255,255,.05)`; the strips stay zero-width unless the slide itself carries an override (timer, per-slide transition, binding, background, muted, loop, mics) - setting the global text transition did not add one | `FS/components/slide/Icons.svelte:110-230` (`RT`) |
| Empty states | "No slides" plus a "New slide" button in the grid; "Nothing here" in an empty playlist | `FS/components/show/Slides.svelte:530-538`, `FS/components/show/ProjectContentList.svelte:404-417` |

Operator controls under the preview, with the titles the running app reports:

| Control | Runtime title | Runtime behaviour |
|---|---|---|
| Hide preview | "Hide preview" | 41x40 button overlaid on the preview, collapses it |
| Previous / Next | "Previous slide [Arrow Left]", "Next slide [Arrow Right]" | both `disabled` at the ends of a show, not hidden |
| Play / Update | "Start show [Space]" off air, "Update output [Ctrl+R]" while live | the same slot swaps meaning |
| Lock | "Lock output [Ctrl+L]" | blocks go, clear and advance |
| Transition | "Transition" | opens the transition popup |
| Clear all | "Clear all [esc]", `class="text clearAll red"` | once cleared the button becomes "Restore output" and drops `red` |
| Per-layer clear | a red 28px clear button stacked over a 15px label button ("Background", "Slide", "Overlays", "Audio", "Next slide timer"); the label button gains `isActive` while that layer is on air | each pair is `disabled` while its own layer is empty, so with only a slide showing just the slide pair is enabled - independent per-layer clearing made visible |

Source: `FS/components/output/ShowActions.svelte:90-118`,
`FS/components/output/preview/ClearButtons.svelte:115-213` (`RT`).

The transition popup is fullscreen chrome (`.popup` with "Close [esc]" and "More
options") holding two tabs, **Text transition** and **Media transition** - matching
the two entries in `transitionData` - and a row of Duration, Flow and Easing
controls. The type list offered is **None, Fade, Blur, Spin, Scale, Slide**;
`crossfade` is absent, as the source said (`T/Show.ts:681`,
`FS/utils/transitions.ts:11-27`) (`RT`).

There is **no "auto go" control**; the closest are a per-slide next timer and a
section trigger action (`FS/components/output/tools/NextTimer.svelte:71-77`,
`FS/components/show/Section.svelte:90-102`).

## 6. Keyboard map

Core shortcuts are hardcoded; there is no remap UI. Per-slide, per-group and
interaction triggers are separate single-key data (`FS/components/output/preview/Preview.svelte:56-77`).

Presenter keys, gated by the preview handler and forwarded to an output window
when that has focus
(`FS/utils/shortcuts.ts:347-439`, `FS/components/output/preview/Preview.svelte:42-117`):

| Key | Action |
|---|---|
| `Space`, `PageDown`, `ArrowRight` | next |
| `PageUp`, `ArrowLeft` | previous |
| `Home` / `End` | go to start / go to end |
| `Esc` or `.` | clear all - `Esc` only when nothing else wants it, see below |
| `F1` | clear background |
| `F2` | clear slide, unless a selection exists (then the app renames instead) |
| `F3` | clear overlays and effects |
| `F4` | clear audio, including playlist and microphones |
| `F5` | **next**, not a clear: `OutputHelper.advanceOutputs()`, a convenience for presenter clickers. The layer the UI calls "Next slide timer" is cleared by its button, not by `F5` (`FS/utils/shortcuts.ts:380-384`, `FS/components/helpers/OutputHelper.ts:20-25`) (`RT`) |
| digits `0-9`, 300ms buffer | play slide N, only when `special.numberKeys`; a fresh install has no `numberKeys` key at all, so **digits do not go to slides by default** (`RT`) |
| a bare letter | play the slide starting with it, only when `special.autoLetterShortcut`, likewise absent by default (`RT`) |

App-level keys (`FS/utils/shortcuts.ts:37-142`): `Ctrl+1…5` switch page,
`Ctrl+1…8` switch drawer tab (body must have focus), `Ctrl+C/X/V/D/A`, `Ctrl+Z`,
`Ctrl+Y`/`Ctrl+Shift+Z`, `Ctrl+S`, `Ctrl+N`, `Ctrl+O` toggle outputs, `Ctrl+R`
update output, `Ctrl+L` lock, `Ctrl+M` mute, `Ctrl+T` toggle panels, `Ctrl+G`/`F8`
quick search, `Ctrl+H` history, `Ctrl+E`/`Ctrl+I` export/import, `Ctrl+Shift+V`
slide view, `Ctrl+Shift+T` text edit mode, `Ctrl+Shift+F` focus mode, `F2` rename,
`F11` fullscreen, `Alt+Enter` split slide text in half.

Gating rules worth copying deliberately:

- Dispatch is split across **two window-level `keydown` listeners**: the app map
  registered by `App.svelte`, and the presenter map registered by the preview
  component (`FS/components/output/preview/Preview.svelte:252`); 17 `keydown`
  listeners are attached to `window` in a running main window (`RT`).
- `Esc` is a priority cascade: quick search → context menu → blur focus → popup →
  selection (`FS/utils/shortcuts.ts:91-126`). The presenter path for `Esc` is
  `setTimeout(clearAll)`, i.e. `clearAll()` with `button = false`, and that
  overload returns early while a popup, selection, context menu or edit selection
  exists. Because the app map clears the selection in a 20ms timeout while the
  presenter map fires at 0ms, **`Esc` right after selecting a slide only deselects
  it**; observed twice in a row with the output still on air, while `.` cleared
  immediately once nothing was selected (`RT`). Treat `Esc` as "cancel the topmost
  thing", not "go black".
- `Ctrl+D` means duplicate when something is selected, otherwise it toggles the
  drawer (`FS/utils/shortcuts.ts:43`, `FS/components/drawer/Drawer.svelte:184-185`).
- `Ctrl+F` means find-and-replace on the edit page, otherwise focus the drawer
  search (`FS/utils/shortcuts.ts:40,144-146`).
- `F2` renames in the app but clears the slide on the output; selection and focus
  mode decide, and the preview handler returns false to let rename proceed
  (`FS/utils/shortcuts.ts:362-370`).
- While typing, only `e i n o s a z y x` (plus `c v` on macOS) still fire
  (`FS/utils/shortcuts.ts:200-205`).
- Arrow keys move output navigation unless edit or stage items are selected
  (`FS/utils/shortcuts.ts:396-409`); arrow keys nudge a selected item 1px, or 10px
  with `Ctrl` (`FS/components/edit/EditTools.svelte:314-326`).
- Keys are normalised to physical position so non-Latin layouts keep working
  (`FS/utils/shortcuts.ts:273-235`).

## 7. Output semantics for our worker

| Behaviour | FreeShow | Source |
|---|---|---|
| Output windows | one frameless always-on-top window per enabled output at the display bounds; **no kiosk or fullscreen API** | `EM/utils/windowOptions.ts:39-56`, `EM/output/helpers/OutputVisibility.ts:132-157` |
| Display selection precedence | keep bounds if the window centre is still on a live display → else the stored screen id unless it would cover over half the main window → else, with `autoPosition` and more than one display, the second display → else configured bounds | `EM/output/helpers/OutputBounds.ts:80-103`, `OutputVisibility.ts:66-113` |
| Display hot-plug | `display-removed` acts immediately, `display-added` after 1s, metrics change after 500ms | `EM/output/helpers/OutputLifecycle.ts:44-78` |
| Identify | numbered full-screen overlays for 3 seconds | `EM/output/helpers/OutputIdentify.ts:8-33` |
| Program state | `OutData { refresh, background, slide, effects[], overlays[], transition, scene }` per output | `T/Output.ts:72-80` |
| Program state as persisted | the serialised buffer is nested in the output object and survives restart, so the app re-presents what was on air: `out: { slide: { id, layout, index, line, revealCount, itemClickReveal }, background, transition, overlays, effects }` | `RT` |
| Output window DOM | `main.background` (so no `--primary` paint) → `div.fill.context.#output_window` (`flex-direction: row`) → `div#<outputId>.zoomed.center` → `div.slide.hideOverflow.landscape` with `background-color: black; transition: 800ms background-color; aspect-ratio: 16/9` → `span.zoom` → the layers; a `.transitioner` wrapper appears only while a transition is running | `FS/MainOutput.svelte:62-71` (`RT`) |
| Canvas scaling | the authored 1920-wide space is mapped with CSS `zoom = boxWidth / 1920`; measured 0.74495 in a 1430px window and 0.13979 in a 268px thumbnail; item geometry and font size stay in authored units (an item at `top:428.50px;left:208.50px;height:220px;width:1500px`, `font-size: 180px` echoed as `--base-font-size`) | `FS/components/slide/Zoomed.svelte:78-80` (`RT`) |
| Cleared output | content is removed from the DOM and the slide box simply shows its own `background-color: black`, fading over the media transition duration (800ms); no blackout layer, no style background left behind | `FS/components/output/Output.svelte:329` (`RT`) |
| Moving a borderless output | the top 5% of the output window is a `-webkit-app-region: drag` strip (`.dragger`), which is how the operator repositions an output that has no frame | `RT` |
| Timers in the background | every window is created with `backgroundThrottling: false` ("allow timers to run in background"), and the renderer process really does run with `--disable-background-timer-throttling --disable-frame-rate-limit --disable-background-media-suspend`; measured in a fully occluded, unfocused output window: 50 ticks of a 100ms interval in 5008ms | `EM/utils/windowOptions.ts:37,68,113` (`RT`) |
| Group colour plumbing | each item carries `--slide-group-color`, `--slide-group-next-color`, `--slide-group-upcoming-color` plus `--variable-number`, so upcoming/next tinting is pure CSS; chords default to `--chord-color: #FF851B` | `RT` |
| Hidden windows still render | an output window created while outputs are off keeps updating its DOM (`visibilityState` stays `visible`), so previews, NDI capture and DevTools inspection all see live content; a screenshot of it needs CDP `fromSurface: false` | `RT` |
| Clear | independent per key: background, slide, overlays, audio, timers, scene, drawing, all | `FS/components/output/clear.ts:13-34,86-198` |
| Restore | clearing snapshots every active output, plus playing audio, so one restore is possible | `FS/components/output/clear.ts:36-84` |
| Slide position | clearing with intent remembers the current slide so the next go continues from it; clearing at the last slide does not | `FS/components/output/clear.ts:97-124` |
| Black | no blackout primitive: black is what remains when the background chain ends, window background `#000000` | `FS/components/output/Output.svelte:329`, `EM/utils/windowOptions.ts:41` |
| Blank | `livePrepare` overlays a pure black layer with a fade while content keeps rendering underneath | `FS/MainOutput.svelte:68-71,114-124` |
| Layer order | style background, scene media, output background, test pattern, effects under slide, underlays, slide, metadata, effects over slide, overlays, attribution, draw | `FS/components/output/Output.svelte:389-461` |
| Layer gating | a style lists which of `background`, `slide`, `overlays` it renders, which is what makes per-layer clear work | `T/Settings.ts:12-15`, `FS/components/output/Output.svelte:401-450` |
| Transitions | `none`, `blur`, `fade`, `fly`, `scale`, `slide`, `spin`; `crossfade` exists in code but is commented out of the UI list | `T/Show.ts:681`, `FS/utils/transitions.ts:11-27` |
| Cut | `type: "none"` or a falsy duration means no transition at all | `FS/components/output/transitions/OutputTransition.svelte:9-16` |
| Transition defaults | text `fade 500ms sine`, media `fade 800ms sine` | `EM/data/defaults.ts:64-67` |
| Transition precedence | slide data → style → global settings; a disable flag forces all three off | `FS/components/helpers/output.ts:1844-1868` |
| **Background continuity** | two slots: if the incoming background has the same path or id it updates in place and does **not** remount or restart; otherwise the new one mounts in the other slot after `duration/4 + 20ms`, cross-dissolves, and the old slot is released on `loaded` with a `max(2000, duration)` fallback | `FS/components/output/layers/Background.svelte:63-74,87-126` |
| Re-playing the same slide | a `refresh` counter bumps and forces a remount | `FS/components/helpers/output.ts:298-306` |
| Slide commit delay | the slide body updates 50ms after the line is decided in an output window (10ms elsewhere) so all outputs agree | `FS/components/output/Output.svelte:234-244` |
| Canvas | authored against 1920x1080 and scaled by ratio to the output resolution | `FS/components/helpers/output.ts:858`, `FS/components/slide/Zoomed.svelte:78-80` |
| Aspect | style aspect ratio, default 16:9; pixel resolution derived from output bounds, 1080-height basis when the output is portrait | `FS/components/helpers/output.ts:820-902` |
| Media fit | `contain`, `cover`, `fill`, `blur` (blur amount 6, opacity 0.3) | `T/Main.ts:186`, `FS/components/slide/getStyleResolution.ts:1-25`, `FS/components/helpers/media.ts:395-426` |
| Text fit | `shrinkToFit`, `growToFit`, `none`, computed size cached on the item | `FS/components/edit/scripts/autosize.ts:9`, `T/Show.ts:114-115` |
| Video controls | play/pause, seek, ±10s, loop, mute, volume; disabled while the output is locked | `FS/components/output/tools/MediaControls.svelte:49-186` |
| Audio | channel data `{volume, isMuted, delay, fadeDuration, dB}` plus a routing graph; default transport rate 48000 | `T/Audio.ts:11-17`, `EM/audio/receiveAudio.ts:10-60` |
| Audio playlist | `{name, songs[], mode default\|shuffle, loop, autoNext on by default, crossfade, volume}` | `T/Audio.ts:1-9` |
| Timers | counter, clock, event and PCO types; default timer is a 5:00 counter; per-slide `timer`, `nextTimer`, `breakDuration` | `T/Show.ts:208-242,378-413`, `EM/data/defaults.ts:110-112` |
| Lock | `outLocked` blocks clear, restore, advance and every go button | `FS/components/output/clear.ts:14,60,190`, `FS/components/output/ShowActions.svelte:90-107` |
| Recreate vs live-set | transparency, invisible and capture transport changes rebuild the output window; bounds, always-on-top and stream data are set live | `FS/components/settings/tabs/Outputs.svelte:76-84`, `EM/output/helpers/OutputValues.ts:9-43` |

## 8. Data model and storage

Containers in the settings store: `SETTINGS`, `SYNCED_SETTINGS`, `SHOWS`,
`STAGE`, `PROJECTS`, `OVERLAYS`, `TEMPLATES`, `EVENTS`, `MEDIA`, `THEMES` plus
caches (`T/Save.ts:95-122`).

| Object | Key fields | Default | Source |
|---|---|---|---|
| Show | `name`, `category`, `reference`, `settings`, `timestamps`, `metadata`, `meta`, `slides`, `layouts`, `media` | `activeLayout: "default"`, `template: "header"` | `T/Show.ts:18-64`, `FS/utils/createData.ts:1316-1355` |
| Slide | `group`, `color`, `globalGroup`, `locked`, `settings`, `children`, `notes`, `items`, `timeline` | — | `T/Show.ts:89-108` |
| Item | `lines`, `type`, `src`, `style` (raw CSS string), `align`, `textFit`, `media`, `fit`, `cropping`, `volume`, `loop`, `bindings`, `actions` | `textFit: shrinkToFit` | `T/Show.ts:110-178` |
| SlideData (playlist entry of a layout) | `id`, `disabled`, `parent`, `children`, `color`, `transition`, `background`, `overlays`, `effects`, `audio`, `mics`, `actions`, `bindings`, `nextTimer`, `end`, `breakDuration` | — | `T/Show.ts:372-414` |
| Group | `name`, `color`, `default`, `template`, `shortcut` | 8 defaults, break/bridge/chorus/intro/outro/pre-chorus/tag/verse, shortcuts B, C, V | `T/Show.ts:196-206`, `EM/data/defaults.ts:5-14` |
| Style | `name`, `layers`, `background`, `backgroundImage`, `transition`, `fit`, `aspectRatio`, `cropping`, `template` | — | `T/Settings.ts:13-45` |
| Project (playlist) | `name`, `created`, `modified`, `used`, `parent`, `shows[]`, `timeline`, `archived` | `{projects:{}, folders:{}, projectTemplates:{}}` | `T/Projects.ts:3-46`, `EM/data/store.ts:37` |
| Output | `enabled`, `active`, `name`, `color`, `bounds`, `screen`, `style`, optional crop, blend, transparency and capture transports | `{enabled: true, active: true, name: "Output", color: "#F0008C", bounds 1920x1080 at 0,0, screen: null}` | `T/Output.ts:8-39`, `EM/data/defaults.ts:16-90` |

Storage: the data root defaults to `Documents/FreeShow` and its folders are fixed
(`EM/utils/files.ts:338-360`, `dataFolderNames`): `Shows`, `Backups`, `Bibles`,
`Online`, `Media`, `Exports`, `Imports`, `Lessons`, `Planning Center`,
`Recordings`, `Audio`, `Cloud` and `Config` - `dataFolderNames.userData` is
literally `"Config"`. One show per file as `Shows/<name||id>.show` whose content is
the JSON tuple `[id, show]` (`EM/data/export.ts:199`); backups as
`Backups/<timestamp>[_auto].zip` containing `PROJECTS.json`, `SHOWS/*` and bibles
(`EM/data/backup.ts:16-17,85-175`). Startup deletes `*.json.tmp-*` leftovers and
drops store files that fail to parse (`EM/data/store.ts:68-107`).

The store files are `<dataPath>/Config/<store>.json`. Measured on a clean run:
`config.json` (only `loaded`, `maximized`, `bounds`, `dataPath`,
`disableHardwareAcceleration`, `graphicsDevice`, `machineId`), plus `settings.json`,
`settings_synced.json`, `projects.json`, `shows.json`, `stage.json`, `themes.json`,
`templates.json`, `overlays.json`, `history.json` and `cache.json`. Passing Electron
a custom `--user-data-dir` keeps the stores there instead and sets
`special.customUserDataLocation: true`; that is how this inspection avoided writing
to a real data root (`RT`).

First run, measured: it creates `Documents/FreeShow/{Bibles,Config,Shows}`, writes
`Shows/Welcome.show`, seeds a "Meetings" project holding one item plus the
Presentations and Songs categories, and the centre of the window offers
`Documentation`, `Quick search`, `New project`, `New show` (`RT`).

Persisted defaults worth copying (`RT`): `resized`
`{leftPanel:290, rightPanel:290, leftPanelDrawer:290, rightPanelDrawer:290}`,
`drawer {height:300, stored:null}`, `drawerTabsData` written only for tabs actually
touched, `slidesOptions {columns:4, mode:"grid"}`, `mediaOptions {columns:5,
mode:"grid"}`, `transitionData` text `fade 500 sine` and media `fade 800 sine`,
`theme "default"`, `autosave "15min"`, `timeFormat "24"`, `language "en_GB"`,
`ports {remote:5510, stage:5511}` with a generated remote password, and a single
output named **"Primary"** - the code default string is "Output" - as
`{enabled:true, active:true, color:"#F0008C", bounds 1920x1080 at 0,0,
screen:null, style:"default"}`.

**There is no schema version field.** Migration is done by
version-named fixup functions and filename-triggered repairs
(`EM/data/store.ts:250-330`, `FS/utils/updateSettings.ts:111-146`). This is the
exact problem our own versioned workspace with a `formatVersion` field avoids.

## 9. FreeShow's operation surface against our planned RPC

FreeShow uses one Electron bridge with a channel enum and a `listenerId` response
correlation and a 15 second timeout (`EM/preload.ts:26-58`,
`T/Channels.ts:1-18`, `EM/IPC/main.ts:9-38`). The output channel's own verb table
is `CREATE`, `REMOVE`, `TOGGLE_OUTPUTS`, `ALIGN_WITH_SCREEN`, `MOVE`,
`UPDATE_BOUNDS`, `SET_VALUE`, `TO_FRONT`, `REQUEST_PREVIEW`, `CAPTURE`,
`IDENTIFY_SCREENS`, `FOCUS` (`EM/output/OutputHelper.ts:16-45`).

| Our initial operation | FreeShow equivalent |
|---|---|
| capabilities | `GET_DISPLAYS`, `GET_GRAPHICS_DEVICES`, `GET_SCREENS` |
| display selection | `output.screen` plus `ALIGN_WITH_SCREEN`, `IDENTIFY_SCREENS` |
| scene preparation | data push then `OUTPUTS`, no explicit prepare step, we add one |
| presentation | `setOutput("slide", …)` then `updateOut` |
| preview requests | `REQUEST_PREVIEW`, `CAPTURE`, plus in-app mirrored previews |
| playback controls | video player API and audio channel data |
| clearing | `clearBackground`, `clearSlide`, `clearOverlays`, `clearAudio`, `clearTimers`, `clearAll`, `restoreOutput` |
| shutdown | `OUTPUT/REMOVE`, window close |

Two operations we should not forget to add because FreeShow's operators clearly
depend on them: **lock output** (`outLocked`, blocks everything) and **restore
after clear** (single-step, snapshot based).

## 10. Scope mapping

Reproduced in the first release: shell with left, center, right and drawer,
playlists, slide grid with grid, list and lyrics modes, previews per output,
presenter keys and per-layer clears, transition defaults, background continuity,
1920x1080 authored canvas, styles and groups.

Deferred, matching `PLAN.md`: stage outputs, layouts, effects, scripture,
calendar, functions, quick search, cloud and content providers, NDI, OMT,
Blackmagic, WebRTC and RTMP, drawing, timers beyond the simple countdown.

## 11. Runtime verification

The ten open questions were taken up against the official 1.6.7 Linux AppImage
(sha512 `MsURzL48zjECRkTTvHnmmtF0fu8QO97PTpFL12kayUw/VGdBrAyUMUh5O3qODa7yYhmG1q9bn4lZ3Q+lQ1GyOQ==`,
165640234 bytes, matching `latest-linux.yml`) driven over CDP.

| # | Question | Result |
|---|---|---|
| 1 | Conditional layout classes | **Settled.** Right panel above 522px becomes `right row` with `flex-direction: row-reverse` (tools inside, preview at the screen edge); the drawer carries its own `#leftPanelDrawer` / `#rightPanelDrawer`. Resize snapping, the 4px/6px handle hit strip, the `zero` collapsed state with its arrow icon, and click-to-restore-290 were all reproduced by dragging (`§2`) |
| 2 | Ghost dimming, group colours, hover strip | **Settled.** `.background.ghost` is `opacity: .4`; hover paints `.hover.overlay` at `rgba(255,255,255,.05)`; group and upcoming colours arrive as `--slide-group-color` / `-next-` / `-upcoming-` custom properties on each item; the per-card `.icons` columns are zero-width unless the slide itself has an override (`§5`, `§7`) |
| 3 | What clear-all leaves | **Settled.** Pure black: the slide box's own `background-color: black` with `transition: 800ms background-color`, content removed from the DOM, no style background behind it. "Clear all" flips to "Restore output" and loses its `red` class (`§5`, `§7`) |
| 4 | Does `crossfade` ever render | **Settled.** No. The transition popup offers exactly None, Fade, Blur, Spin, Scale, Slide; `crossfade` is not offered anywhere in the UI |
| 5 | Pixels of the `blur` fit and edge blending | **Not verified.** Needs a controlled image and video asset; the operator route to a background is a drag from the Media drawer, and the only images on this machine are personal screenshots. Deferred to the M4 acceptance pass with our own fixtures |
| 6 | Drag feedback | **Settled from the shipped CSS**, which is what the browser actually applies: `.droparea.hover` fades the target to `opacity: .3` over `transition: opacity .3s` behind a `rgba(0,0,0,.4)` wash, dropping onto an output preview draws `border: 3px solid var(--secondary)` on `.previewContainer::after`, and a chord drop target gets `outline: 2px dashed var(--secondary)` with `rgba(255,255,255,.15)` |
| 7 | Handler dispatch order | **Partly settled, and it matters.** Two window-level `keydown` listeners compete: the app map and the presenter map from `Preview.svelte`; 17 `keydown` listeners are attached to `window` overall. `Esc`'s presenter path is `setTimeout(clearAll)` (0ms) while the app map defers deselecting by 20ms, so `Esc` with a selection never clears the output - confirmed by pressing it twice with the output on air, where `.` cleared immediately. Numbers in the 300ms buffer are the slide-N path. Exact interleaving of the other listeners was not traced |
| 8 | Per-slide action side effects | **Not verified.** The action list is documented from source; exercising timed actions on go needs a purpose-built show file. Deferred to M4 |
| 9 | Timer accuracy under throttling | **Settled.** All windows set `backgroundThrottling: false`; a 100ms interval in a fully occluded, unfocused output window fired 50 times in 5008ms |
| 10 | Real persisted settings | **Settled.** First-run artefacts and every default recorded in `§8`, including that digit-to-slide and letter-to-slide are off unless the user turns them on |

Two things the run gave us that the source alone did not: the output window's top
5% is an `-webkit-app-region: drag` strip, and a hidden output window keeps
rendering live DOM updates, which is why previews and capture work with the window
off screen.

## 12. How these captures were made

Not part of the build; kept for reproducibility. The driver is committed as
`tools/cdp-inspect.mjs` (`node tools/cdp-inspect.mjs <command> <target> [args]`).

1. `curl` the AppImage into `third_party/cache/`, `chmod +x`, verify
   `openssl dgst -sha512 -binary … | openssl base64 -A` against `latest-linux.yml`.
2. Run it with `--remote-debugging-port=9222
   --user-data-dir=/tmp/opencode/fs/userdata`, so its stores stay out of
   `Documents/FreeShow`. Chrome/138, Electron/37.10.3.
   The app's own stdout is worth reading too: it prints a staged GPU probe
   (`[GPU-STATUS]` at t=0, 10s and 25s for compositing, rasterization, webgl and
   video decode, then `[GPU-HEALTH]` with the VA drivers found and a decode probe
   result). That is a good pattern for our worker's first-run diagnostics.
3. `/json/list` returns two page targets with the same title, and the order is not
   stable: identify them by feature (`[class*="output_window"]` for the output
   window, `.drawer` for the main one) and address them by target id. Note the
   class is literally `#output_window`, so `querySelector('#output_window')` fails;
   match on the attribute instead.
4. `Page.captureScreenshot` works on the main window but stalls on an off-screen
   output window unless `fromSurface: false`; captures also stall once when a
   window is occluded, so `Page.bringToFront` plus a short retry loop is worth it.
   `Emulation.setDeviceMetricsOverride` fakes viewport sizes, but panel widths need
   a real drag, and FreeShow's resize handler ignores coalesced moves: synthetic
   drags must send ~20 discrete `mouseMoved` events with small delays.
5. Screenshots are reference material, not repo content: 18 PNGs are kept in
   `third_party/cache/freeshow-visuals/`, which is Git-ignored.
