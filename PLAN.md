# OpenShowQt: resumable implementation and Git checkpoints

Accepted: 2026-10-21. Status: implementation started; see [PROGRESS.md](PROGRESS.md).

## Goal and reference

Build a Linux-first presentation application that closely reproduces FreeShow's
core operator screens using a new C++ architecture. The first usable release
supports service playlists, lyrics, a basic visual editor, images, video, audio,
operator previews and one audience display.

Reference: `/home/jim/FreeShow`, FreeShow 1.6.7, commit
`436d4b634878ba723da1d1373637384ec700ce16`. Leave the source clone unchanged.

Allow **12–16 focused engineer-weeks**, assuming one engineer. Re-estimate after
M1; target an early lyric demonstration around weeks 4–5. Estimates are not
completion promises.

## Fixed architecture and interfaces

- **Frontend:** Qt 6 Quick/QML, with C++ models, document commands, persistence
  and worker supervision. Reproduce the show workspace, editor, library drawer
  and relevant settings.
- **Domain:** Qt-independent C++ library for shows, slides, groups, reusable
  styles, media and playlists. Qt model adapters expose it to QML.
- **Backend:** a separate **Qt-free C++ worker**, using SDL3 for window/display/
  audio handling, Skia Ganesh with Vulkan for composition, SkParagraph for text,
  and FFmpeg libraries for decoding, pixel conversion and audio resampling.
  The worker owns its Vulkan instance/device, swapchain and synchronization.
- **Scheduling:** separate decoding, RPC handling and rendering. Do not block
  presentation or audio callbacks on files, decoding, IPC or preview consumers.
- **RPC:** JSON-RPC 2.0 over Unix-domain sockets, using Qt local sockets in the
  frontend and standalone Asio in the worker. Length-prefixed messages,
  protocol-version negotiation, request IDs, structured errors and acknowledged
  scene revisions. Share Qt-independent scene/protocol types using nlohmann/json.
- **Initial operations:** capabilities, display selection, scene preparation/
  presentation, preview requests, playback controls, clearing and shutdown.
  Events report applied revisions, playback position, displays, statistics and
  failures. Live indicators reflect acknowledged worker state.
- **Preview:** POSIX shared-memory slots, outside RPC. Three slots with explicit
  frame-ready/release messages; drop previews under load rather than blocking
  output. Begin with 640×360 at 15 fps and on-demand editor previews/thumbnails.
  Never overwrite a slot still owned by the consumer.
- **Ownership:** the frontend owns editable documents and all writes. Compile
  documents into immutable render scenes with resolved asset references. The
  worker owns playback/output and lives only while connected to its frontend.
- **Recovery:** worker restart begins black and requires an explicit presentation
  action. Display removal disables output until a destination is selected.
- **Storage:** versioned JSON workspace with stable IDs, explicit geometry/styles,
  shows, playlists and a media manifest. Copy media to a sibling assets directory
  and use relative references. Atomic saves, separate recovery autosave, undo/redo.
- **Initial scope:** Linux, one audience output, SDR, stereo audio, multiple text/
  image boxes, move/resize/layer ordering, typography, backgrounds, groups,
  reusable styles, cuts/crossfades, images/video and embedded video audio mixed
  with one standalone audio track. Initially use software decoding and GPU
  composition. Video holds its final frame at completion.
- **Editor:** worker-rendered imagery with QML selection handles; text entry
  through an inspector keeps final text layout in Skia.
- **Deferred:** FreeShow import/round-trip compatibility, stage displays,
  scripture, additional outputs, hardware decoding, advanced effects, remote
  control, streaming, NDI/SDI, cloud integrations, Windows and macOS.

Technical references:

- [Qt local sockets](https://doc.qt.io/qt-6/qlocalsocket.html)
- [Asio local sockets](https://think-async.com/Asio/asio-1.30.2/doc/asio/overview/posix/local.html)
- [SDL Vulkan surfaces](https://wiki.libsdl.org/SDL3/SDL_Vulkan_CreateSurface)
- [SDL PCM output](https://wiki.libsdl.org/SDL3/SDL_OpenAudioDeviceStream)
- [Skia device/context integration](https://skia.org/docs/user/api/skcanvas_creation/)

## Task, pause and Git rules

1. Execute milestones in order; tasks follow their listed order. Independent
   preparation may proceed when an environment blocker prevents an earlier
   task, but do not declare the milestone complete or start dependent features.
2. Aim for tasks lasting half a day to two days. Split larger tasks into numbered
   subtasks before starting them. Record dependencies and acceptance checks.
3. Use `TODO`, `IN PROGRESS`, `BLOCKED`, `DONE`. Mark done only after acceptance
   checks pass; separate authored work from runtime verification.
4. Each completed task must be independently reviewable and commit-ready. Keep
   unfinished features disabled or behind development options.
5. Before every pause update `PROGRESS.md`: current task ID, working changes,
   checks/results, decisions, blockers, remaining work and exact next action.
6. At each milestone run its checks and propose a commit message plus a file
   summary. The user controls commits and tags unless explicitly delegating them.
7. Commit documentation with implementation. Exclude build output, fetched
   dependencies, caches, personal settings and media. Do not stage unrelated work.
8. **Resume:** read this file and `PROGRESS.md`; inspect Git status/history and
   existing changes; continue the recorded task without overwriting other work.

Every task depends on the preceding task unless explicitly split for independent
preparation. Detailed acceptance evidence belongs in `PROGRESS.md`.

## Work breakdown

### M0 — Planning, references and build foundation

| ID | Status | Work | Acceptance |
|---|---|---|---|
| 0.1 | DONE | Write plan and progress files | Both contain agreed architecture and task ledger |
| 0.2 | TODO | Establish Git metadata and ignore rules | Git status works; generated files are excluded |
| 0.3 | TODO | Capture core screens, controls and shortcuts | Reference specification covers agreed screens |
| 0.4 | TODO | Establish CMake targets and dependencies | UI and Qt-free worker skeletons build reproducibly |
| 0.5 | TODO | Record dependency versions and asset notices | Skia revision/settings and dependencies are recorded |

Checkpoint: `M0: establish project plan and build foundation`.

### M1 — Prove frontend-to-renderer integration

| ID | Status | Work | Acceptance |
|---|---|---|---|
| 1.1 | TODO | SDL/Vulkan window and Skia rendering | Text and image render correctly |
| 1.2 | TODO | Worker launch, RPC handshake and shutdown | Commands change output; disconnect stops worker |
| 1.3 | TODO | Display selection and swapchain recreation | Fullscreen, resizing and switching work |
| 1.4 | TODO | Shared-memory previews | Qt displays worker frames without blocking output |
| 1.5 | TODO | Video decode and A/V benchmark | Results establish feasibility on this PC |

Checkpoint: `M1: prove rendering worker and preview pipeline`.
Resolve integration failures before expanding the application.

### M2 — Documents and application state

| ID | Status | Work | Acceptance |
|---|---|---|---|
| 2.1 | TODO | Shows, slides, groups, styles and playlists | Domain operations pass focused tests |
| 2.2 | TODO | Persistence and asset copying | Sample service survives save/reopen |
| 2.3 | TODO | Undo/redo and recovery autosave | Edits reverse; interrupted sessions recover |
| 2.4 | TODO | Qt adapters and scene compilation | Edits produce valid immutable scenes |

Checkpoint: `M2: add persistent presentation documents`.

### M3 — First functional lyric presentation

| ID | Status | Work | Acceptance |
|---|---|---|---|
| 3.1 | TODO | Text layout and typography | Wrapping, alignment, fallback and fit pass fixtures |
| 3.2 | TODO | Image/background composition, transitions | Layering, cuts and crossfades render correctly |
| 3.3 | TODO | Minimal lyric entry and navigation | Newly created song can be presented |
| 3.4 | TODO | Output controls and acknowledged live state | Clear controls/live indicators match output |

Checkpoint: `M3: deliver working lyric presentation`.
First demonstration: create, save and present a short lyric service. Clearing
foreground, background, audio or everything must be independent. Preserve a
background video across lyric changes when its asset/settings remain unchanged.

### M4 — Core FreeShow-style workspace and editor

| ID | Status | Work | Acceptance |
|---|---|---|---|
| 4.1 | TODO | Main workspace and resizable panels | Layout matches agreed reference captures |
| 4.2 | TODO | Searchable libraries, playlist ordering | Shows/media can be found and arranged |
| 4.3 | TODO | Object selection, move and resize | Geometry edits render correctly; support undo |
| 4.4 | TODO | Typography, layering, reusable styles | Styles apply consistently across slides |
| 4.5 | TODO | Shortcuts, clipboard, essential settings | Core flows work by keyboard and mouse |

Checkpoint: `M4: complete core operator workspace and editor`.

### M5 — Video and audio

| ID | Status | Work | Acceptance |
|---|---|---|---|
| 5.1 | TODO | Bounded FFmpeg decoding queues | Fixtures decode without growing queues |
| 5.2 | TODO | Video controls, seeking and looping | Playback transitions/seek flushing pass tests |
| 5.3 | TODO | Standalone audio and stereo mixing | Video/music have independent volume/mute |
| 5.4 | TODO | Playback clocks and synchronization | A/V timing meets acceptance targets |
| 5.5 | TODO | Preload, continuous backgrounds, errors | Lyrics preserve playback; failures are visible |

Checkpoint: `M5: integrate synchronized video and audio playback`.
Use the audio clock when audio is present and a monotonic clock otherwise.
Test PNG/JPEG, H.264/AAC MP4, MP3 and WAV; report unavailable formats/codecs.

### M6 — Reliability and installable alpha

| ID | Status | Work | Acceptance |
|---|---|---|---|
| 6.1 | TODO | Worker/device/display failure handling | Documents intact; output can restart safely |
| 6.2 | TODO | Playback/rendering diagnostics | Timing, dropped frames and queues inspectable |
| 6.3 | TODO | Package application | Both executables/resources run on clean Fedora |
| 6.4 | TODO | Service rehearsal and acceptance fixes | Complete acceptance suite passes |

Checkpoint: `M6: deliver installable presentation alpha`.

## Acceptance and verification

Record actual verification commands/results for every task. Use focused tests
for domain behaviour, persistence and IPC, rendering fixtures for layout, and
physical hardware for display/media acceptance.

- Documents: round-trips, undo/redo, interrupted saves, recovery, missing media,
  unsupported document versions.
- RPC: fragmented/invalid messages, version mismatch, disconnect and restart.
- Rendering: fixed fonts, Unicode, scaling, fit modes, transitions and preview/
  output consistency.
- UI: agreed captures at fixed sizes; keyboard navigation, drag/drop, mixed DPI.
- Media: play/pause/seek/loop, silent video, video audio plus music, malformed
  files and end-of-file behaviour.
- Performance: 1080p60 composition with source-rate video, under 1% missed
  presentation deadlines in ten minutes on this PC with previews enabled.
- Responsiveness: preloaded slide changes within 100 ms at the 95th percentile.
- Synchronization: steady A/V offset within 50 ms over thirty minutes.
- Reliability: one-hour mixed service without growing queues, sustained memory
  growth or unexpected output changes.
- Platforms: physical-display checks on Fedora Wayland and X11. Headless checks
  supplement these and cannot replace them.

Each milestone handoff records what works, remaining limitations, passing checks
and the next task. Never claim physical or runtime verification from static checks.
