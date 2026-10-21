# FreeShow 1.6.7 operator reference

Derived from the source clone, not from the running application. FreeShow 1.6.7,
commit `436d4b634878ba723da1d1373637384ec700ce16` at `/home/jim/FreeShow`.
Electron main process plus a **Svelte** renderer (547 `.svelte`, 466 `.ts`).

Citations are `path:line` relative to that clone. Shorthands: `FS` =
`src/frontend`, `EM` = `src/electron`, `T` = `src/types`. Sections marked
**Needs runtime** are the residual questions that only a running copy can
settle; see the consolidated list at the end.

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
| Splitter | handle 4px, default width 290, max 638, snap: below 200.1 closes to 4, 200.1–290 snaps to 200.1, 270–310 snaps to 290; clicking a closed handle restores 290 | `FS/components/system/Resizeable.svelte:18-22,69-81,94-102`, `FS/utils/common.ts:14` |
| Right panel goes two-column | when its width exceeds 522px the preview and tools sit side by side | `FS/MainLayout.svelte:77,171-182` |
| Top bar | height 40, drag region, title at left (`width: var(--navigation-width)`), page buttons centred, settings and output buttons right | `FS/components/main/Top.svelte:94-158` |
| Unsaved marker | 5px red strip at the left edge of the top bar while unsaved | `FS/components/main/Top.svelte:99-101,205-211` |
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
`FS/components/drawer/Drawer.svelte:28-30,81-111,184-185`). Search sits in the tab
bar at `--navigation-width`, `Ctrl+F` focuses it, `Esc` clears
(`FS/components/drawer/Drawer.svelte:164-227,287-292`). Shows and Media lists are
virtualised (`FS/components/drawer/pages/Shows.svelte:272`,
`FS/components/drawer/media/Media.svelte:616`).

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
| Thumbnail | 1-based number at the left of the label bar; active slide gets a 2px accent outline, edit selection a 5px text-colour outline | `FS/components/slide/Slide.svelte:277,448,546-552`, `FS/components/system/SelectElem.svelte:331,364` |
| Click to go | a plain single click on a slide sends it to the output immediately; `Ctrl`/`Shift` click only selects; `Alt` goes without extras | `FS/components/show/Slides.svelte:96-99,143-144` |
| Hover icon strip | per-slide icons for disabled, timer, next timer, transition, output binding, background, muted, loop, mics, each clickable to remove | `FS/components/slide/Icons.svelte:110-230` |
| Empty states | "No slides" plus a "New slide" button in the grid; "Nothing here" in an empty playlist | `FS/components/show/Slides.svelte:530-538`, `FS/components/show/ProjectContentList.svelte:404-417` |

Operator controls under the previews, with their titles:

| Control | Title in FreeShow | Action |
|---|---|---|
| Previous / Next | "Previous slide [Arrow Left]", "Next slide [Arrow Right]" | advance outputs |
| Play | "Start show [Space]" | first enabled slide of the active show |
| Update | "Update output [Ctrl+R]" | re-push current scene (replaces Play while live) |
| Lock | "Lock output" / "Unlock output [Ctrl+L]" | blocks go, clear and advance |
| Transition | "Transition" | per-slide transition popup |
| Clear all | "Clear all [esc]" then "Restore output" | clears everything, restorable once |
| Per-layer clear | "Clear background [F1]", "Clear slide [F2]", "Clear overlays [F3]", "Clear audio [F4]", "Clear timer [F5]" | independent per layer |

Source: `FS/components/output/ShowActions.svelte:90-118`,
`FS/components/output/preview/ClearButtons.svelte:115-213`.

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
| `Esc` or `.` | clear all |
| `F1` `F2` `F3` `F4` `F5` | clear background, slide, overlays, audio, timers |
| digits `0-9`, 300ms buffer | play slide N, only when `special.numberKeys` |
| a bare letter | play the slide starting with it, only when `special.autoLetterShortcut` |

App-level keys (`FS/utils/shortcuts.ts:37-142`): `Ctrl+1…5` switch page,
`Ctrl+1…8` switch drawer tab (body must have focus), `Ctrl+C/X/V/D/A`, `Ctrl+Z`,
`Ctrl+Y`/`Ctrl+Shift+Z`, `Ctrl+S`, `Ctrl+N`, `Ctrl+O` toggle outputs, `Ctrl+R`
update output, `Ctrl+L` lock, `Ctrl+M` mute, `Ctrl+T` toggle panels, `Ctrl+G`/`F8`
quick search, `Ctrl+H` history, `Ctrl+E`/`Ctrl+I` export/import, `Ctrl+Shift+V`
slide view, `Ctrl+Shift+T` text edit mode, `Ctrl+Shift+F` focus mode, `F2` rename,
`F11` fullscreen, `Alt+Enter` split slide text in half.

Gating rules worth copying deliberately:

- `Esc` is a priority cascade: quick search → context menu → blur focus → popup →
  selection (`FS/utils/shortcuts.ts:91-126`).
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

Storage: data root `Documents/FreeShow` with `Shows`, `Backups`, `Bibles`, `Media`,
`Config` and others; one show per file as `Shows/<name||id>.show` whose content is
the JSON tuple `[id, show]`; config as `Config/<store>.json`; backups as
`Backups/<timestamp>[_auto].zip` containing `PROJECTS.json`, `SHOWS/*` and bibles
(`EM/utils/files.ts:338-360`, `EM/data/export.ts:199`, `EM/data/store.ts:30-53`,
`EM/data/backup.ts:16-17,85-175`). Startup deletes `*.json.tmp-*` leftovers and
drops store files that fail to parse (`EM/data/store.ts:68-107`).

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

## 11. Needs runtime to confirm

1. Effective composition of conditional layout classes: the right panel
   two-column threshold and the drawer split views.
2. Real appearance of the ghost background dimming, group colour bars and the
   hover icon strip on slides.
3. What clear-all leaves behind in practice: pure black, or the style background.
4. Whether `crossfade` and the `between` transition variants ever render.
5. Pixel results of the `blur` fit mode and output edge blending.
6. Drag feedback when moving items between the drawer and the playlist.
7. Relative dispatch order of the many window-level key handlers, and the
   20-300ms `setTimeout` races around `Esc`, duplicate and `F2`.
8. Per-slide action side effects on go, which are timing dependent.
9. Timer tick accuracy under background throttling.
10. Actual persisted user settings such as panel widths and enabled tabs, which
    are not in the repository.

Capturing these needs a running copy. The plan is the official 1.6.7 Linux
AppImage driven over the Electron DevTools protocol, which gives window
enumeration, DOM and computed-style reads, synthetic input and per-window
screenshots without extra system packages.
