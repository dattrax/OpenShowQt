# OpenShowQt: resumable implementation and Git checkpoints

Accepted: 2026-10-21. Status: implementation started; see [PROGRESS.md](PROGRESS.md).

## Goal and reference

Build a Linux-first presentation application that closely reproduces FreeShow's
core operator screens using a new C++ architecture. The first usable release
supports service playlists, lyrics, a basic visual editor, images, video, audio,
operator previews and one audience display.

Reference: `/home/jim/FreeShow`, FreeShow 1.6.7, commit
`436d4b634878ba723da1d1373637384ec700ce16`. Leave the source clone unchanged.

In service the operator PC drives two screens: the operator UI on the first, the
projector as the **second** display carrying the audience output. Our default
output target is therefore the second display, which is also what FreeShow's
`autoPosition` rule picks (`docs/reference.md` §7). The projector is not connected
to this machine right now, so output-window behaviour is currently only observable
off screen; see the output-window log in `PROGRESS.md`.

Allow **12–16 focused engineer-weeks**, assuming one engineer. Re-estimate after
M1; target an early lyric demonstration around weeks 4–5. Estimates are not
completion promises.

## Fixed architecture and interfaces

- **Frontend:** Qt 6 Quick/QML, with C++ models, document commands, persistence
  and worker supervision. Reproduce the show workspace, editor, library drawer
  and relevant settings.
- **Domain:** Qt-independent C++ library for shows, slides, groups, reusable
  styles, media and playlists. Qt model adapters expose it to QML.
- **Backend:** a separate **Qt-free C++ worker**, using GLFW for its output
  window and Vulkan surface, vk-bootstrap for instance and device setup, Skia
  Ganesh with Vulkan for composition, SkParagraph for text, and FFmpeg
  libraries for decoding, pixel conversion and audio resampling. The worker
  owns its Vulkan instance/device, swapchain and synchronization.
- **Scheduling:** separate decoding, RPC handling and rendering. Do not block
  presentation or audio callbacks on files, decoding, IPC or preview consumers.
- **RPC:** JSON-RPC 2.0 over Unix-domain sockets, using Qt local sockets in the
  frontend and a single-threaded `poll()` loop on a raw Unix socket in the
  worker. Length-prefixed messages, protocol-version negotiation, request IDs,
  structured errors and acknowledged scene revisions. Share Qt-independent
  scene/protocol types using Boost.JSON.
- **Initial operations:** capabilities, display selection, scene preparation/
  presentation, preview requests, playback controls, clearing and shutdown.
  Events report applied revisions, playback position, displays, statistics and
  failures. Live indicators reflect acknowledged worker state.
- **Preview:** POSIX shared-memory slots, outside RPC. Three slots with explicit
  frame-ready/release messages; drop previews under load rather than blocking
  output. Begin with 640×360 at 15 fps and on-demand editor previews/thumbnails.
  Never overwrite a slot still owned by the consumer.
- **Audio:** native PipeWire. The worker mixes embedded video audio and one
  standalone track in software to a single F32 stereo stream at the negotiated
  rate, with independent volume and mute per source. Its process callback is
  the playback clock whenever audio is running. Never mix or block on PipeWire
  from the render or RPC threads.
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
  scripture, additional outputs, no-compositor output through the Vulkan
  display plane, hardware decoding, advanced effects, remote
  control, streaming, NDI/SDI, cloud integrations, Windows and macOS.

### Output layer model (added 2026-10-22)

The audience output composites each frame from ordered layers, bottom to top:
**background** (colour, gradient, image or video), **images**, **words**,
**aux** (auxiliary overlays) and **clock**, mirroring FreeShow's output tabs.
**Audio is deliberately not a layer**: as in FreeShow it is a playback source
owned by the AV subsystem, never a composition item dispatched to Skia.

- Compositing is **one pass with no render-to-texture**: every enabled layer
  draws straight into the current swapchain surface in z order. No per-layer
  or intermediate render targets — the target PC is low-end. Transparency
  goes directly into paint alphas on the shared target.
- A disabled layer is **functionally skipped**: its draw entry point is never
  called, so it records no Skia work and costs nothing.
- Frontend can **toggle any layer at runtime** (over RPC from task 1.2 on);
  the change takes effect on the next frame with no swapchain or GPU-object
  recreation. Aux and clock are not wanted for this operator's use case and
  start off in production output; they stay implemented so later overlay
  kinds follow the same shape.
- The worker's `LayerStack`/`RenderLayer` (`src/worker/render/Layer.{h,cpp}`)
  implements this, the task 1.1 test scene is built from one layer per tab,
  and `openshow-worker --check-layers` (ctest `render_layer_composition`)
  proves on the CPU that disabled layers contribute nothing and each layer
  paints only its own region.

### Worker architecture graduation (added 2026-10-23)

The task 1.1 worker is deliberately scaffolding: `runRenderTest()` in
`src/worker/main.cpp` is a fixed development loop, and
`render/TestScene.cpp` holds five hard-coded layer classes, the CPU snapshot
harness and the layer checks in one file. That shape was acceptable as
proof, not as a destination. The decomposition is not a separate cleanup
milestone — a dedicated refactor task before the protocol and scene types
exist would design APIs by guesswork — it is driven by task acceptance, and
each step below names the piece of scaffolding that must graduate:

- **1.2** introduces the worker **runtime seam**: a session object that owns
  the output window, swapchain, `LayerStack` and mutable output state, and
  runs the frame loop. RPC handlers call session operations; they must not
  reach into test-scene state. `--render-test` becomes a development mode
  whose demo scene feeds the session — the session gets a first client
  before the socket does — and the frame-30 readback probe moves behind a
  diagnostics flag inside the session.
- **1.3** moves display selection behind session operations driven by RPC
  (list displays, select destination, recreate at the configured extent),
  replacing the `--monitor` flag's placement logic.
- **1.4** renders a scene into an arbitrary target — offscreen 640x360 for
  preview slots as well as the swapchain — the first hard requirement that
  the scene be data the renderer walks rather than code.
- **2.4** delivers the real input type: immutable compiled render scenes as
  Qt-independent shared types over the protocol. The worker stores prepared
  scenes with acknowledged revisions; the 1.x demo content becomes a
  fixture that builds one.
- **3.1/3.2** replace the five hard-coded layers with production layers that
  read scene data (text, images, backgrounds and fit modes, transitions).
  `TestScene` shrinks to a test fixture whose snapshot and
  `verifyLayerComposition` harnesses live in the test tree.
- **6.1** owns worker/device restart so the session can be destroyed and
  rebuilt without touching the device layer.

Enforcement rule for reviews: a task does not pass by threading a command
through to hard-coded scene state. If a required command cannot be answered
by calling a session or scene operation, the seam is introduced first, as
part of that task.

Technical references:

- [Qt local sockets](https://doc.qt.io/qt-6/qlocalsocket.html)
- [Boost.JSON](https://www.boost.org/doc/libs/1_90_0/doc/html/json.html)
- [GLFW Vulkan support](https://www.glfw.org/docs/3.4/vulkan.html)
- [PipeWire media streams](https://docs.pipewire.org/page_handling_media_sets.html)
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
| 0.2 | DONE | Establish Git metadata and ignore rules | Git status works; generated files are excluded |
| 0.3 | DONE | Capture core screens, controls and shortcuts | Reference specification covers agreed screens |
| 0.4 | DONE | Establish CMake targets and dependencies | UI and Qt-free worker skeletons build reproducibly |
| 0.5 | DONE | Record dependency versions and asset notices | Skia revision/settings and dependencies are recorded |

Checkpoint: `M0: establish project plan and build foundation`.

#### Recorded dependencies (task 0.5, this machine)

- **Skia:** `/home/jim/skia` at `a94df1cdab`, static `out/Release/libskia.a`
  built with `skia_use_vulkan=true`, `skia_use_gl=false`,
  `is_official_build=true` and internal libpng, libjpeg, zlib, expat and
  FreeType. `libskparagraph.a`, `libskshaper.a`, `libskunicode_core.a` and
  `libskunicode_icu.a` are already built, so SkParagraph needs no rebuild; PNG
  and JPEG decoding come from SkCodec inside Skia.
- **Proven pattern:** `/home/jim/sloppy-ui` opens a GLFW window with no client
  API, creates the Vulkan surface, builds a `GrDirectContext` and presents
  through its own `Swapchain` and `SwapchainImage` classes with per-image
  fences and semaphores. Start the worker from that pattern.
- **Installed:** GLFW 3.4.0, Vulkan loader and headers, Boost 1.90 (with
  Boost.JSON), Qt 6.11.2 base and declarative development packages,
  `pipewire-devel` 1.6.9, `ffmpeg-devel` 9.0.2, CMake, Ninja, GCC.
  vk-bootstrap `v1.3.225` arrives through FetchContent.
- **FFmpeg gotcha:** its headers carry no `extern "C"` guards, so every C++
  translation unit that includes them must wrap them, or the symbols will not
  resolve at link time.
- **Licences to ship:** Skia (BSD-3), GLFW (zlib), vk-bootstrap (MIT),
  Boost.JSON (BSL-1.0), PipeWire (MIT), FFmpeg (LGPL or GPL depending on
  configure flags), Qt 6 (LGPL/GPL). Collected with the package in task 6.3.
- **No shader compiler needed.** Skia's Vulkan backend turns SkSL into SPIR-V
  in process through its own code generator (`SkSL::ToSPIRV`, a member of
  `libskia.a`), which is why `/home/jim/sloppy-ui` renders with no shaders of
  its own. A custom transition or colour effect would be SkSL for
  `SkRuntimeEffect`, still compiled by Skia. Shader tooling such as spirv-cross
  or RenderDoc is for debugging only.
- **Sockets and JSON, decided 2026-10-21:** the worker uses a `poll()` loop on
  a raw Unix socket and Boost.JSON for all payloads. `asio-devel` and
  `nlohmann-json-devel` are therefore not installed and not wanted.

### M1 — Prove frontend-to-renderer integration

| ID | Status | Work | Acceptance |
|---|---|---|---|
| 1.1 | DONE | GLFW/Vulkan window and Skia rendering | Text and image render correctly |
| 1.2 | DONE | Worker launch, RPC handshake and shutdown | Commands change output through the runtime seam (see graduation note); disconnect stops worker |
| 1.2a | DONE | Session seam: frame loop out of `main.cpp` into `OutputSession` | `--render-test` presents unchanged; all checks green |
| 1.2b | DONE | Wire framing and JSON-RPC envelope in `openshow::protocol` | Round-trip, fragmentation, oversize and malformed-input tests pass |
| 1.2c | DONE | Worker RPC server thread on `--socket PATH` | Handshake completes; worker exits on `shutdown` and on disconnect |
| 1.2d | DONE | Live RPC commands driving the seam | A socket command changes visible output on the next frame |
| 1.2e | DONE | Frontend supervision: spawn, handshake, shutdown | UI shows worker state; closing the UI terminates the worker |
| 1.3 | DONE | Display selection and swapchain recreation | Fullscreen, resizing and switching work |
| 1.3a | DONE | Session display operations: list and select applied on the render thread | Selecting a destination moves the output and rebuilds the swapchain at the new extent |
| 1.3b | DONE | RPC surface: `output.displays`, `output.select`, monitor fields in status | A socket command moves the output between panels |
| 1.3c | DONE | Frontend destination picker | Dropdown in the UI moves the audience output |
| 1.3d | DONE | Acceptance: both panels, automated switch test | Scene demonstrably eDP→DP-4→windowed→back, seen; interactive drag-resize noted as manual |
| 1.4 | DONE | Shared-memory previews | Qt displays worker frames without blocking output (see 1.4a–e) |
| 1.4a | DONE | `preview_wire.h` in the protocol library: shm segment layout, per-slot seqlock headers, ready/release message shapes | Layout and notification shapes covered in protocol tests; one Qt-free definition compiled by both processes |
| 1.4b | DONE | Session preview producer: same scene drawn to a 640x360 GPU offscreen surface (same Vulkan backend as the output) at 15 fps, read back into free shm slots | Wire-verified: a live frame lands in a slot; owned slots are never overwritten; stalls count as drops |
| 1.4c | DONE | RPC control: `preview.start`/`preview.stop`, server-originated `preview.ready` notification, `preview.released` from the client, capability and status fields | Socket client mmaps the named segment, decodes frames, stalls to raise drops, stops and restarts (ctest scenario D) |
| 1.4d | DONE | Frontend consumer: mmap the segment, copy ready slots to a QImage under the seqlock, send releases, PreviewView item in the UI | Preview pane showed the live scene ("77 frames shown · 0 dropped"); drawImage fit fix verified after |
| 1.4e | DONE | Acceptance: automated (frame content follows layer toggles; a stalled consumer raises drops while the output keeps presenting) and seen on screen | ctest scenario D green; preview live in the UI |
| 1.5 | IN PROGRESS | Video decode and A/V benchmark | Results establish feasibility on this PC |
| 1.5a | DONE | `media/` skeleton: single `extern "C"` FFmpeg header, `MediaProbe` (streams, codecs, fps, duration, colour info, decoder availability) | `media_tests` probes a generated H.264/AAC fixture; structured errors for missing/unparseable files; fixtures skip (exit 77) when no ffmpeg CLI |
| 1.5b | TODO | Decode thread: native I420 into a bounded PTS-tagged staging ring, claim/release/drop semantics (`FrameRing`) | Sustained decode ≥ 2× source fps on a 1080p fixture; high-water bounded; EOF/loop reported; drops counted |
| 1.5c | TODO | PipeWire sink + resampled feed + audio clock (delivered frames / negotiated rate) | Minutes-long fed run, zero underruns; callback interval p99 and negotiated rate recorded |
| 1.5d | TODO | YUV GPU upload pipeline: device-local optimal-tiling plane ring on the transfer queue family, copy + `TRANSFER_DST→SHADER_READ` + fence on the upload thread, one `SkImages::TextureFromYUVATextures` wrap per slot | Headless Vulkan test: synthetic I420 pattern composites pixel-correct through the pre-wrapped image; upload ms instrumented separately |
| 1.5e | TODO | Presentation scheduler: event-driven frame loop (eventfd + display fd + scheduled wakes) in `OutputSession`; `--av-bench` development mode presents clocked video slots | Static slide: present counter flat and ~0% CPU over 10 s idle; a command or a new video slot wakes the next vsync; preview publishes only on content change |
| 1.5f | TODO | Benchmark runs; feasibility numbers into PROGRESS.md | 2–3 representative files with the preview on, incl. one ~10-min soak; verdict against M5 targets (<1% missed deadlines, <50 ms drift) |

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
| 3.2 | TODO | Image/background composition, transitions | Layering, cuts and crossfades render correctly; layer on/off from the UI skips dispatch per the output layer model |
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
| 5.3 | TODO | PipeWire output and stereo mixing | Video and standalone audio have independent volume and mute |
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
- Displays: any check that needs an audience output — 1.3 display selection and
  swapchain recreation, output placement, fit modes and transitions on a real
  panel — requires the projector on the second display. With it unplugged those
  checks stay open; evidence gathered from an off-screen output window is logged
  and labelled as such, and does not satisfy them.
- Platforms: physical-display checks on Fedora Wayland and X11. Headless checks
  supplement these and cannot replace them.

Each milestone handoff records what works, remaining limitations, passing checks
and the next task. Never claim physical or runtime verification from static checks.
