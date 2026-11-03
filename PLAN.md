# OpenShowQt: implementation plan

Accepted 2026-10-21; reviewed and condensed 2026-10-30.
Current checkpoint and verification: [PROGRESS.md](PROGRESS.md).
Current code boundaries and open differences: [docs/architecture.md](docs/architecture.md).
Detailed completed-task evidence: [history](docs/history/README.md).
Post-M6 conformance review: [original findings](docs/reviews/M6-conformance-2026-10-30.md).
Architecture review: [after 6.22](docs/reviews/M6-post-6.22-review-2026-10-31.md); [6.23 closure evidence](docs/reviews/M6-6.23-verification-2026-10-31.md).

## Goal and reference

Build a Linux-first C++/Qt presentation application closely reproducing FreeShow's
core operator screens: service playlists, lyrics, visual editing, images, video,
audio, operator previews and one audience display.

Reference: `/home/jim/FreeShow`, FreeShow 1.6.7 at
`436d4b634878ba723da1d1373637384ec700ce16`. Leave that clone unchanged.
Main-screen visual reference (2026-10-31): `third_party/cache/propresenter-visuals/`.
Layout behaviour: [ProPresenter UI guide](https://support.renewedvision.com/hc/en-us/articles/360041345954-Understanding-The-ProPresenter-User-Interface), with notes in [the reference](docs/reference.md).
Use charcoal/blue, library and running order left, slides centre, audience right.
Show presents; slide authoring and destructive document shortcuts belong to Edit.

The normal operator setup uses the second display for the projector; when it is
unplugged, physical-output checks remain open. The original 12–16 engineer-week
estimate is historical; re-estimate remaining work before productization.

## Fixed architecture and interfaces

- **Frontend:** Qt 6 Quick/QML with C++ document/model adapters, persistence and
  worker supervision. It owns all editable documents and package writes.
- **Domain and scenes:** Qt-independent C++20 Documents/History/storage plus
  immutable compiled scenes with resolved styles, geometry and local asset paths.
  Boost.JSON is the shared JSON implementation.
- **Worker:** separate Qt-free process owning GLFW output, Vulkan instance/device,
  swapchain and synchronization; vk-bootstrap setup, Skia Ganesh/Vulkan composition,
  SkParagraph with real ICU text support, FFmpeg decode/conversion/resampling.
- **Scheduling:** decoding, RPC and rendering have separate execution contexts.
  Files, decode, IPC and preview consumers must not block presentation or audio
  callbacks. The handoff and scheduling gaps tracked in 6.22 closed on 2026-10-31
  (6.22a–g all DONE).
- **RPC:** length-prefixed JSON-RPC 2.0 on Unix-domain sockets; Qt local sockets
  in the frontend and a raw socket/poll loop in the worker. Version/capability
  negotiation, request IDs, structured errors and acknowledged revisions. Traffic
  carries bounded scene/control metadata and paths, never original/decoded media.
- **Preview:** composed worker pixels through three POSIX shared-memory slots;
  exact stream/sequence ready-release ownership. Normally 640×360 at up to 15 fps
  on content changes; selected editor scenes and on-demand thumbnails are bounded
  separately. Drop/defer previews under load; never overwrite a consumer-owned slot.
- **Audio:** native PipeWire, F32 stereo at negotiated rate, independent gain/mute
  for video and one standalone track. Two independent production streams are the
  accepted design (6.20d); audible pairing and relative latency remain 7.4.
  Audio is the clock when running, monotonic otherwise.
- **Output state:** SceneStore owns prepared scenes and desired/landed/published
  commands. The frame loop captures once; status only credits a successfully
  presented frame. A separate SceneFrame fixes scene/blends/time/revision for
  drawing the audience and its mirror preview.
- **Recovery:** a restarted worker begins black and requires explicit presentation.
  Destination removal must disable output until explicitly reselected. Complete
  crash/device/display recovery remains **7.1a–c**.
- **Storage:** versioned JSON service directory with stable IDs and sibling assets;
  relative package references, staged durable saves, separate recovery autosave,
  bounded undo/redo. Background autosave is implemented; explicit saves still
  execute on the UI thread.
- **Editor:** worker-rendered selected scene beneath QML selection handles; text
  entered in inspectors, with final text layout owned by Skia.
- **Scope:** Linux, one SDR audience output, stereo audio, multiple text/image
  items, move/resize/order, typography, groups, reusable styles, backgrounds,
  cuts/crossfades and media playback. Video holds its final frame when not looping.
  Software decode is supported; optional VA-API and direct Vulkan-backed software
  decode were added during M1, with an explicit copying fallback.
- **Bible import:** online, passage-only lookup into today's schedule, with
  theme-sized slides; planned in BI below. No installed Bible or passage cache.
- **Deferred:** FreeShow import/round-trip, stage/additional outputs,
  Vulkan display-plane output, advanced effects, remote control, streaming,
  NDI/SDI, cloud services, Windows and macOS.

### Output layer model

The original control model is **background → images → words → aux → clock**;
aux/clock default off in production. Disabled layers skip drawing. Audio is a
playback source, never a composition layer. Runtime toggles take effect without
GPU/swapchain recreation.

Audience composition draws directly into one target with no per-layer targets
or `saveLayer`; fades use shader/paint composition. Preview/snapshot targets and
cached CPU blur backdrops are separate resources. Skia application dependencies
use public core/Ganesh/module APIs only. Source guards enforce these constraints.

Foreground images and text share one authored item stack. The images and words
controls filter that stack by kind without changing its order (6.20d).

### Responsibility boundaries

OutputSession coordinates window/device/swapchain, frame capture and presentation.
SceneRenderer owns layers/transitions; SceneStore owns command/scene truth.
Preview production, background transport, frontend RPC/consumer/presentation and
save/UI components have separate owners; worker supervision remains 7.1a.
Development fixtures must drive those production seams; commands must not
manipulate hard-coded demo state. Keep a single document/history owner and a single worker text renderer.

Skia is pinned in `third_party/skia`; the build recipe and configuration live in
`scripts/build-skia.sh` and `cmake/OpenShowSkia.cmake`. Dependency setup, system
versions and old probe results are in the archive; package notices/build manifest
remain **7.3**. Do not use old machine paths as current dependency configuration.

## Task, pause and Git rules

1. Work in milestone/dependency order. Preserve recorded DONE checkpoints;
   later reviews add focused follow-up tasks rather than rewriting earlier evidence.
   The architecture follow-ups (6.22) closed 2026-10-31 (6.22a–g all DONE); M7
   work is no longer gated on them.
2. Aim for half a day to two days per leaf; split larger tasks before starting.
   A parent is DONE only when every child passes its acceptance.
3. Use TODO, IN PROGRESS, BLOCKED and DONE. Separate authored work, passing tests
   and physical verification. M0–M5 are historical implementation checkpoints,
   not proof that the later review gaps or M7 release gates are closed.
4. Keep each task independently reviewable and commit-ready; unfinished features
   stay disabled or behind development options. Preserve unrelated working files.
5. Before pausing update PROGRESS.md with current task, changes, checks, blockers
   and exact next action. Keep it a handoff; put lasting contracts in architecture
   documentation and long completed logs in history, without duplicating the ledger.
6. At milestones run acceptance and propose a commit message/file summary. The user
   controls commits/tags unless explicitly delegated. Include docs with code;
   exclude generated output, fetched dependencies, caches and personal/media files.
7. Resume by reading PLAN.md/PROGRESS.md and inspecting Git status/history.
8. Keep PLAN.md concise and stable: update status and at most a 2–4-line summary
   per completed task (a brief deliverable and acceptance reference in table rows).
   Do not append implementation narratives, timings, command output or commit-by-
   commit accounts. Keep PROGRESS.md to a short current handoff; put lasting
   contracts in architecture documentation and detailed completed evidence in history.
   Add or split work items only when scope requires it, with brief descriptions.

## Work breakdown

Each DONE row records the deliverable and regression entry point. Historical
measurements and detailed acceptance are preserved in [the archive](docs/history/README.md).
The current audit distinguishes architecture matches from remaining differences.

### M0 — Planning, references and build foundation

| ID | Status | Work | Acceptance |
|---|---|---|---|
| 0.1 | DONE | Write plan and progress files | Both contain agreed architecture and task ledger |
| 0.2 | DONE | Establish Git metadata and ignore rules | Git status works; generated files are excluded |
| 0.3 | DONE | Capture core screens, controls and shortcuts | Reference specification covers agreed screens |
| 0.4 | DONE | Establish CMake targets and dependencies | UI and Qt-free worker skeletons build reproducibly |
| 0.5 | DONE | Record dependency versions and asset notices | Skia revision/settings and dependencies are recorded |

Checkpoint: `M0: establish project plan and build foundation`.

### M1 — Prove frontend-to-renderer integration

| ID | Status | Work | Acceptance |
|---|---|---|---|
| 1.1 | DONE | GLFW/Vulkan window and Skia rendering | Text and image render correctly |
| 1.2 | DONE | Worker launch, RPC handshake and shutdown | Runtime seam and acknowledged commands; disconnect stops worker |
| 1.2a | DONE | Session seam: frame loop out of `main.cpp` into `OutputSession` | `--render-test` presents unchanged; all checks green |
| 1.2b | DONE | Wire framing and JSON-RPC envelope in `openshow::protocol` | Round-trip, fragmentation, oversize and malformed-input tests pass |
| 1.2c | DONE | Worker RPC server thread on `--socket PATH` | Handshake completes; worker exits on `shutdown` and on disconnect |
| 1.2d | DONE | Live RPC commands driving the seam | A socket command changes visible output on the next frame |
| 1.2e | DONE | Frontend supervision: spawn, handshake, shutdown | UI shows worker state; closing the UI terminates the worker |
| 1.3 | DONE | Display selection and swapchain recreation | Automated switching verified historically; projector checks remain M7 |
| 1.3a | DONE | Session display operations: list and select applied on the render thread | Selecting a destination moves the output and rebuilds the swapchain at the new extent |
| 1.3b | DONE | RPC surface: `output.displays`, `output.select`, monitor fields in status | A socket command moves the output between panels |
| 1.3c | DONE | Frontend destination picker | Dropdown in the UI moves the audience output |
| 1.3d | DONE | Automated display-switch acceptance | Historical eDP/DP/windowed checks; manual drag-resize remains open |
| 1.4 | DONE | Shared-memory previews | Live composed frames; ownership and stalled-consumer coverage |
| 1.4a | DONE | Shared preview layout and notification types | `protocol_tests`, `preview_stream_tests` |
| 1.4b | DONE | GPU offscreen preview producer, 640×360 at up to 15 fps | Free-slot-only publication; drops under consumer pressure |
| 1.4c | DONE | Preview RPC lifecycle and ready/release notifications | `worker_rpc_lifecycle` |
| 1.4d | DONE | Frontend mmap consumer and PreviewView | `preview_ownership_tests`, `production_output_tests` |
| 1.4e | DONE | Preview acceptance under content changes and stalls | `worker_rpc_lifecycle`; historical on-screen verification |
| 1.5 | DONE | Video decode and A/V feasibility benchmark | Historical ten-minute feasibility soak; full M7 acceptance still required |
| 1.5a | DONE | FFmpeg wrapper, media probing and structured failures | `media_tests`; generated fixtures skip visibly without ffmpeg CLI |
| 1.5b | DONE | Decode thread and bounded PTS frame ring | Decode throughput, boundedness, EOF and loop checks |
| 1.5c | DONE | PipeWire sink, stereo feed ring and audio clock | `audio_mix_tests`, `av_sync_tests`; live daemon coverage is environment-dependent |
| 1.5d | DONE | Device-local YUV upload ring and Skia wrappers | `gpu_yuv_tests`; full rings refuse overwrite |
| 1.5e | DONE | Event-driven presentation scheduler and clocked AV bench | `render_idle_check`; historical zero-idle-presents check |
| 1.5f | DONE | Preview-enabled benchmark matrix and soak | Historical worst missed deadlines 0.11%; timing evidence in archive |
| 1.5g | DONE | Opt-in Vulkan validation layers and debug messenger | GPU fixtures with `OPENSHOW_VULKAN_VALIDATION=1` |
| 1.5h | DONE | Optional VA-API decode with software fallback | Effective decoder logged; `--no-hw-decode` forces software |
| 1.5i | DONE (compatibility path) | Copying YUV staging fallback, explicitly permitted by startup option | GPU sampler/staging fixtures; software High10 conversion |
| 1.5j | DONE | Direct software decode into pooled Vulkan images | `gpu_decode_tests`; bounded decoder pool and presentation ring |

Checkpoint: `M1: prove rendering worker and preview pipeline`.

### M2 — Documents and application state

| ID | Status | Work | Acceptance |
|---|---|---|---|
| 2.1 | DONE | Qt-independent document model and stable IDs | `domain_tests` |
| 2.2 | DONE | Versioned service packages and copied assets | `domain_tests`; durability/containment refined in 6.12–6.13 |
| 2.3 | DONE | Bounded snapshot undo/redo and recovery autosave | `domain_tests`, `document_controller_tests` |
| 2.4a | DONE | Immutable scene type, compiler and validating JSON codec | `scene_tests`; resolved styles, geometry and asset references |
| 2.4b | DONE | Prepare/present worker store with acknowledged revisions | `output_status_tests`; ownership now in SceneStore (6.18a1) |
| 2.4c | DONE | Qt scene adapters and compiled demo fixtures | `document_present_tests`, `worker_controller_tests` |

Checkpoint: `M2: add persistent presentation documents`.

### M3 — First functional lyric presentation

| ID | Status | Work | Acceptance |
|---|---|---|---|
| 3.1 | DONE | SkParagraph scene text layout and font fallback | `scene_render_tests`, `unicode_text_tests`; real ICU added in 6.2b |
| 3.2 | DONE | Image backgrounds and media/text cuts/crossfades | `scene_render_tests`, `render_scene_transition`; compositing corrected in 6.11b |
| 3.3 | DONE | Lyric authoring, navigation and document session | `document_controller_tests`, `document_present_tests` |
| 3.4 | DONE | Independent clears, restore and acknowledged live indicators | `scene_edit_tests`, `worker_controller_tests`; intent/retention refined in 6.3b |

Checkpoint: `M3: deliver working lyric presentation`.

### M4 — Core FreeShow-style workspace and editor

| ID | Status | Work | Acceptance |
|---|---|---|---|
| 4.1 | DONE | Resizable workspace shell and persisted panel widths | `workspace_panels_tests`; historical fixed-size UI capture |
| 4.2 | DONE | Searchable libraries, playlist ordering | Shows/media can be found and arranged |
| 4.3 | DONE | Object selection, move and resize | Geometry edits render correctly; support undo |
| 4.4 | DONE | Typography, layering, reusable styles | Styles apply consistently across slides |
| 4.5 | DONE | Shortcuts, clipboard, essential settings | Core flows work by keyboard and mouse |

Checkpoint: `M4: complete core operator workspace and editor`.

### M5 — Video and audio

| ID | Status | Work | Acceptance |
|---|---|---|---|
| 5.1 | DONE | Production bounded decode/presentation pipeline | `playback_queue_tests` |
| 5.1a | DONE | PlaybackPipeline owns decode, upload and paced submission | CPU and GPU paths share the production pipeline |
| 5.1b | DONE | AV benchmark drives PlaybackPipeline | Historical live preview-enabled benchmark; diagnostics extraction landed with 6.18b2 |
| 5.1c | DONE | Queue-bound and parked-stop regression fixtures | `playback_queue_tests` |
| 5.2 | DONE | Production video playback controls | `playback_control_tests`; seek mapping corrected in 6.7d |
| 5.2a | DONE | Decode-thread seek flush and pacing-generation drain | `playback_control_tests` |
| 5.2b | DONE | Pause parking and resume deadline re-anchoring | `playback_control_tests` |
| 5.2c | DONE | Live loop toggle and paused-seek behavior | `playback_control_tests` |
| 5.3 | DONE | Stereo mixing with independent source gain/mute | `audio_mix_tests`; production two-stream decision in 6.20d |
| 5.3a | DONE | Bounded per-input rings and clickless gain ramps | `audio_mix_tests` |
| 5.3b | DONE | Mix through real file feeders | `audio_mix_tests`; analytical RMS and starvation checks |
| 5.3c | DONE | Preview-enabled live two-source PipeWire bench | Historical zero-underrun run; daemon-dependent acceptance |
| 5.4 | DONE | AvPlayer transport and playback-clock orchestration | `av_sync_tests`; daemon-dependent cases report skips |
| 5.4a | DONE | Inactive sink start, pause/resume and clock anchoring | `av_sync_tests` |
| 5.4b | DONE | Audio seek flush, rebase and acknowledgement seam | `av_sync_tests`; failures/generation pairing refined in 6.7d |
| 5.4c | DONE | AvPlayer pre-roll, lifecycle and clock fallback | `av_sync_tests`; restartability refined in 6.7a/b |
| 5.4d | DONE | Clock, feeder and complete transport regressions | `av_sync_tests`; thirty-minute hardware gate remains M7 |
| 5.5 | DONE | Prepared assets, continuous video backgrounds and failures | `background_media_tests`; scheduling follow-up 6.21c |
| 5.5a | DONE | Video background identity in compiled scenes | `scene_tests` |
| 5.5b | DONE | Production background transport and lyric continuity | `background_media_tests` |
| 5.5c | DONE | Prepare-time still decode and video probe | `background_media_tests`; bounded executor in 6.9a |
| 5.5d | DONE | Sticky acknowledged media errors and notifications | `background_media_tests`, `protocol_tests` |
| 5.5e | DONE | Live continuity, media-swap and preload acceptance | Historical p95 18.7 ms; fresh target-machine gates remain M7 |

Checkpoint: `M5: integrate synchronized video and audio playback`.

### M6 — Code review remediation and architecture

The [original review](docs/M6-code-review.md) introduced R1–R19; productization
moved to M7. Recorded completion through 6.23 remains DONE. The
[post-6.22 review](docs/reviews/M6-post-6.22-review-2026-10-31.md) introduced five
async integration findings, closed by 6.23 with [regression evidence](docs/reviews/M6-6.23-verification-2026-10-31.md).
M7 release gates remain open. Historical 6.1–6.4 reliability references mean 7.1–7.4.

| ID | Status | Focused work | Acceptance / review finding |
|---|---|---|---|
| 6.0 | DONE | Architecture and RPC review; focused remediation ledger | [Original R1–R19 findings](docs/M6-code-review.md); source audit in [architecture.md](docs/architecture.md) |
| 6.1a | DONE | Black production startup; aux/clock/diagnostics off by default | `production_output_tests`, `render_production_compositor`; R1 |
| 6.1b | DONE | Production SceneRenderer; opt-in demo decoration/harness | Production/pixel tests; worker dev-target split in 6.18b2; R1 |
| 6.2a | DONE | Public Skia GPU APIs, owned VMA allocator and revision pin | `skia_public_api_guard`, GPU/YUV tests; R9 |
| 6.2b | DONE | Public SkUnicode/ICU; remove homemade Unicode shim | `unicode_text_tests`; zero direct private Skia includes; R9 |
| 6.2c | DONE | Pinned Skia submodule and repeatable build recipe | `scripts/build-skia.sh`, `cmake/OpenShowSkia.cmake`; R9 |
| 6.3a | DONE | Correlate replies by request ID and connection generation | `worker_controller_tests`; latest-go and stale-reply checks; R3 |
| 6.3b | DONE | Compose clear intent; retain restore content until confirmed | `worker_controller_tests`, `scene_edit_tests`; R3 |
| 6.4 | DONE | Publish output state only for the frame actually presented | `output_status_tests`, `scene_store_tests`; R4 |
| 6.5 | DONE | Stream-specific, sequence-exact preview ownership | `preview_stream_tests`, `preview_ownership_tests`; R5 |
| 6.6a | DONE | Displayed-video leases and sample synchronization | GPU/ownership fixtures; R6 |
| 6.6b | DONE | Quiesce producers before transport/presenter replacement | `background_media_tests`, playback fixtures; R6 |
| 6.7a | DONE | Restartable mixer, sink, ring and transport state | `audio_mix_tests`, `av_sync_tests`; R2 |
| 6.7b | DONE | Standalone source identity and sinkless clock semantics | `audio_mix_tests`, `av_sync_tests`; R2 |
| 6.7c | DONE | Typed decoder, feeder, sink and background failure state | Media/AV/background fixtures; foreground worker status added in 6.20d; R18 |
| 6.7d | DONE | Paired seek generations, failure reports and shared deadlines | `playback_control_tests`, `av_sync_tests`, `background_media_tests`; R19 |
| 6.8a | DONE | Encoded-size, scene/reference and JSON-RPC envelope limits | `protocol_tests`, `scene_tests`, `worker_rpc_limits`; R11 |
| 6.8b | DONE | Bounded nonblocking RPC writers and stall handling | `outbound_queue_tests`, `worker_rpc_transport`, `worker_controller_tests`; R11 |
| 6.9a | DONE | Bounded off-RPC scene preparation | `prepare_executor_tests`, `background_media_tests`; thumbnail follow-up 6.21d; R10 |
| 6.9b | DONE | Prepared video, text and blur resources before go | `background_media_tests`, `scene_frame_tests`; safety/scheduling follow-ups 6.21a/c; R10/R16 |
| 6.10 | DONE | Budgeted image cache, input limits, pins and retry | `scene_image_cache_tests`, `background_media_tests`; pinned excess reported, not evicted; R12 |
| 6.11a | DONE | One immutable audience frame; previews cannot advance fades | `scene_frame_tests`, `preview_budget_tests`; synchronous readback remains; R13 |
| 6.11b | DONE | Correct background fade groups without intermediate targets | `scene_render_tests`, `render_layer_guard` and negative fixtures; R13 |
| 6.12 | DONE | Adopt saved/recovered assets across model/history/scene references | Domain/controller regressions; R7 |
| 6.13a | DONE | Staged durable asset/manifest commit and explicit I/O failures | Domain interruption/fault regressions; complete bytes guaranteed, strict version rollback not promised; R8 |
| 6.13b | DONE | Loaded ID validation and symlink-aware asset containment | `domain_tests`; R8 |
| 6.14a | DONE | Visible autosave/clear failures; flush edits on exit | `document_controller_tests`; R14 |
| 6.14b | DONE | Bounded coalesced background autosave and reusable asset copies | Domain/controller regressions; explicit save/load still synchronous; R14 |
| 6.15a | DONE | Typed production playback, seek, loop, gain and mute controls | Protocol/controller/background regressions; R15 |
| 6.15b | DONE | Standalone track selection with independent transport lifetime | `background_media_tests`; two-stream decision in 6.20d; R15 |
| 6.16a | DONE | Worker-rendered selected-scene editor preview | `preview_selection_tests`, `worker_controller_tests`; R16 |
| 6.16b | DONE | Bounded worker thumbnails over preview transport | `preview_selection_tests`, `ui_loads`; R16 |
| 6.17a1 | DONE | Stable multi-text-box model, storage and compiler | Domain/scene/controller regressions; legacy migration; R17 |
| 6.17a2 | DONE | Editor text-box selection, creation, geometry and ordering | `document_controller_tests`, `scene_render_tests`, `ui_loads`; R17 |
| 6.17b1 | DONE | Foreground images in model, storage, wire and ordered draw | Domain/scene/pixel regressions; layer-kind filtering in 6.20d; R17 |
| 6.17b2 | DONE | Editor image creation, asset choice, geometry and ordering | `document_controller_tests`, `scene_render_tests`, `ui_loads`; R17 |
| 6.17c1 | DONE | Cover/contain background fits end to end | Domain/scene/pixel regressions; unknown fit names refused; R17 |
| 6.17c2 | DONE | Cached blur-fit still backdrop; refuse blur on video | Image-cache/pixel regressions; prepare-time prewarm in 6.9b; R17 |
| 6.17c3 | DONE | Vertical background gradient through model/storage/render/editor | Domain/scene/controller/pixel regressions; gamma-encoded sRGB interpolation; R17 |
| 6.18a1 | DONE | SceneStore owns prepared scenes and desired/landed/published state | `scene_store_tests`, `output_status_tests`; immutable sharing optimized 2026-10-29; R1/R4/R5/R10/R13 |
| 6.18a2 | DONE | Extract PreviewProducer ownership and budget policy | `preview_producer_tests`; live preview regressions; R5/R11/R13 |
| 6.18b1 | DONE | Extract BackgroundTransport and its media status | `background_transport_tests`, `background_media_tests`; R6/R10/R15 |
| 6.18b2 | DONE | Separate development drivers from the product worker | Dev CLI/pixel gates and both Qt-free binary guards; R10 |
| 6.19a1 | DONE | Extract frontend RpcClient transport and request ledger | Unchanged `worker_controller_tests`; R3/R5/R11 |
| 6.19a2 | DONE | Extract frontend PreviewConsumer and thumbnail cache | Controller/live preview regressions; R5/R11/R16 |
| 6.19a3 | DONE | Extract PresentationModel scene intent and restore state | Controller/document-present regressions; R3/R5/R16 |
| 6.19a4 | DONE | Separate worker RPC connection from method/status codecs | RPC lifecycle/transport/limits and output-status regressions; R3/R11 |
| 6.19b1 | DONE | Extract document save/autosave coordination | Unchanged persistence/controller regressions; R7/R14 |
| 6.19b2 | DONE | Extract document list and selection projections | Controller/search/workspace regressions; R16/R17 |
| 6.19b3 | DONE | Extract window keyboard shortcuts | UI load guard; key-table alignment checked by reading; R17 |
| 6.19b4 | DONE | Extract output dock and shared panel controls | UI load and document-present regressions; R13/R15/R16 |
| 6.19b5 | DONE | Extract playlist dock | UI load, workspace and controller regressions; R16 |
| 6.19b6 | DONE | Extract show page and follow mode | UI load, controller and document-present regressions; R14/R16 |
| 6.19b7 | DONE | Extract edit page and canvas actions | UI load, scene-edit and controller regressions; R17 |
| 6.19b8 | DONE | Extract settings page and mirrored shortcut cheat sheet | UI load/workspace regressions; completes 6.19b; R17 |
| 6.20a | DONE | Record R1–R19 evidence matrix and remaining M7 gates | [Architecture matrix](docs/architecture.md#review-closure-r1r19-evidence-matrix-task-620a) |
| 6.20b | DONE | Recheck public Skia guard and bounded-RPC audit | Public-API positive/negative guards; protocol/scene/RPC fixtures |
| 6.20c | DONE | Production worker journey and idle-write deadline correction | `production_journey_tests`, transport regressions; integration follow-up 6.21g |
| 6.20d | DONE | Resolve layer controls, audio contract and foreground worker status | Scene pixel/output-status regressions; frontend follow-up 6.21f |
| 6.20e | DONE | Record M6 closure evidence and environment limits | [Architecture closure](docs/architecture.md#m6-closure-the-completion-rule-in-writing); archived 6.20d gates |

Checkpoint: `M6: close code review and graduate production architecture`.

### M6 follow-up — Conformance review (6.21)

| ID | Status | Work | Acceptance |
|---|---|---|---|
| 6.21a | DONE | Keep drawn layouts alive across concurrent preparation | Retained layout handles; cache-churn regression; F1 lifetime fixed, shared shaping safety in 6.22a |
| 6.21b | DONE | Adopt foreground paths in all retained scene payloads | Controller adoption fixtures and real-worker journey; F2 |
| 6.21c1 | DONE | Stage held audio readiness before presentation | Slow-start background-media fixture; F3 readiness half |
| 6.21c2 | DONE | Move video joins to bounded retirement; stage standalone starts | Single-switch timing fixtures; remaining safety/overlap/state gaps in 6.22b–f |
| 6.21d | DONE | Decode thumbnail files off the RPC thread | Slow-decode responsiveness fixtures; stale-result compatibility in 6.22g; F5 |
| 6.21e | DONE | Clear gradient backgrounds and report their live state | Scene-edit/controller and live pixel fixtures; F4 |
| 6.21f | DONE | Show acknowledged foreground-picture failures in the UI | Controller projection/repair and shell-load fixtures; F6 |
| 6.21g | DONE | Exercise the real frontend against a supervised worker | `frontend_journey_tests`: presentation, clears, restore, adoption and repair; F7 |

### M6 follow-up — Architecture re-review (6.22)

Historical A1–A7 and their fixes are recorded in the previous architecture review
and archived 6.22 handoff. The current post-6.22 findings are B1–B5 below.

| ID | Status | Work | Acceptance |
|---|---|---|---|
| 6.22a | DONE | Give mutable shaping resources safe thread ownership | Per-thread FontCollection and concurrent shaping/handoff fixtures; A1 |
| 6.22b | DONE | Synchronize GPU teardown with queue submissions | Shared queue guard at all three device-wait sites; GPU ordering fixtures; A2; fence retirement remains M7.1b |
| 6.22c | DONE | Deactivate outgoing audio at retirement handoff | Paused sink and frozen-delivery fixture across slow retirement; A3 |
| 6.22d | DONE | Defer background swaps when retirement capacity is occupied | Transport-level overlap fixtures; session publication follow-up 6.23b; A4 |
| 6.22e1 | DONE | Carry standalone duration with the staging result | Slow metadata-probe fixture; no publish-time file read; A5 |
| 6.22e2 | DONE | Probe standalone selections off the RPC thread | Deferred accept/refusal and RPC responsiveness fixtures; removal ordering follow-up 6.23c; A5 |
| 6.22e3 | DONE | Retire replaced standalone players off rendering | Silent handoff, slow-stop and capacity fixtures; ownership/outcome follow-ups 6.23a/e; A5 |
| 6.22e4 | DONE | Wait for audio seeks off rendering | Single-flight completion/retirement fixture; continuing-playback clock follow-up 6.23d; A5 |
| 6.22f | DONE | Publish standalone ownership and pending status together | Slow replacement and fault-owner fixtures; A6 |
| 6.22g | DONE | Match thumbnail completions to their destination | Generation/extent compatibility across resize and stream restart; A7 |

### M6 follow-up — Async integration review (6.23)

| ID | Status | Work | Acceptance |
|---|---|---|---|
| 6.23a | DONE | Make standalone supersession and retirement reservation one decision | Controlled request-arrival race cannot overwrite a busy lane or lose a joinable thread; B1 |
| 6.23b | DONE | Commit deferred scenes and media as one output transaction | Real-session A→B→C/clear retains truthful pending/active state and matching preview pixels; B2 |
| 6.23c | DONE | Invalidate old standalone probes when a newer intent removes audio | Removal during one or several queued probes cannot resurrect playback; B3 |
| 6.23d | DONE | Pair seek completion with video deadline and clock generations | Delayed forward/backward seeks continue presenting without stale-clock stalls; B4 |
| 6.23e | DONE | Distinguish superseded audio stages from failed ones | Successful supersession emits no media error; genuine current-stage failures remain visible; B5 |

### Operator workspace — Main-display layout (UI)

Current priority: match the supplied operator layout, keeping setup in Settings
and authoring in Edit. [Detailed scope, control audit and acceptance](docs/ui/operator-workspace-plan.md).
The UI leaves are complete; the next workspace feature is planned in
[Themes: editor, apply and restore](docs/ui/themes-plan.md) (section below).

| ID | Status | Work | Acceptance |
|---|---|---|---|
| UI.1 | DONE | Remove setup and diagnostic forms from Show | Routing/content setup lives in Settings; Show keeps runtime cues and concise notices; the visible-tree audit finds no setup control or writable field but `librarySearchField` on Show and exposes the picker there; picker URLs with spaces/`#`/Unicode reach the wire decoded while non-local URLs cannot alter the track or library; the shell loads without QML warnings and stays usable at the three capture sizes; visible-tree and file-picker regressions |
| UI.2 | DONE | Sidebar collection outline and selected contents | One outline (Library + playlists, counts) above one detail view; selection, duplicate entries, long names, scrolling and drag/drop keep stable entry identities, and the wire proves browsing never goes live |
| UI.3 | DONE | Presentation/playlist slide workspace | Closed by UI.3a–c: the running order drives the grid, click/key presents and browsing never does, markers wait for the worker's acknowledgement, and every size the view control picks asks a bounded band that keeps its destination and extent |
| UI.3a | DONE | Read-only running-order cue projection | Cue groups per entry (duplicate shows are two entry ids over shared slides); arbitrary-show compilation resolves the owning show and never moves the editing cursor |
| UI.3b | DONE | Consistent Show cue triggers and live markers | Grid walks the running order across presentation boundaries (keys clamped at the ends, "Page Down"/"Page Up" spelling fix makes PgUp/PgDn real); click/key presents, browse never does; PENDING/LIVE markers wait for the worker's acknowledgement and the journey test ties LIVE to the acknowledged pixels |
| UI.3c | DONE | Thumbnail sizing and supported view controls | The heading's −/+ steps the cell (five steps, step 2 is the 190 px the grid had) and the level persists clamped; the ask follows the tile at 16:9 inside the 32..512/640×360-slot envelope, dedupes repeated entries, hits the 32-scene cap exactly and re-anchors at the end of the order; a burst of zoom/scroll ticks costs one ask at the destination, and no size button steals the presenter's keys |
| UI.4 | DONE | Dedicated bottom media bin | Closed by UI.4a–b: the bottom of the window is a media bin whose browsing never touches the output, and its one cue control activates the production audio track under the worker's probe |
| UI.4a | DONE | Media collection/cue browsing and type metadata | The bottom is a media bin: kind collections with counts, one domain-owned kind per row, and the import filter spelled from the same table; filtering, selecting, importing and removing never present or rewrite a slide, Add to slide is offered only for the kind it places, an unusable name says why (or steps aside when the row is narrow), and closing or resizing the bin preserves sidebar selection, slide navigation and the monitor |
| UI.4b | DONE | Explicit media cue activation | Play/Stop on the audio row is `media.standalone` and nothing else — it costs no undo step, moves no cursor or cue marker and presents nothing, and is offered only where a runtime exists (a video's route stays a slide background authored in Edit). The worker validates playback off this process: a row whose file has gone says "file is not here", a probe refusal marks that row "cannot play" naming the file on Show and quoting the worker in Edit, an ON AIR marker follows the status block alone, and the removal is the stop |
| UI.5 | DONE | Runtime preview, clear rail, transport and audio area | The rail is the plan's diagram: per-layer clear pairs beside the audience picture with Clear all/Restore, the acknowledged video transport beneath (with its source's gain and mute), and an AUDIO TRACK area whose cues are the bin's prepared audio by domain kind — picking browses, Play activates the production track through the same confirmed command as the bin, Stop is the removal; the interim file chooser and the controller's URL entry point are gone, no routing or layer setup reappeared, and every control's enablement answers for itself |
| UI.6 | DONE | Toolbar, responsive layout and visual acceptance | The header is Search · Show · Edit · Media visibility left and audience state · Settings right, all live on every page; Search is exactly Ctrl+F and the toggle mirrors the bin's own state; pinned keyboard handback, drag cancellation and strip restore; deterministic captures at 1000/1380/1680 in both bin states plus long names, empty collections and the disconnected output, with the reference differences recorded |

### Themes — editor, apply and restore (TH)

Theme editor in a header submenu, themes applied to songs, customizations
restorable. Priority: slide customization → current theme → slide theme →
show default → built-in. [Detailed model, format 6 and acceptance](docs/ui/themes-plan.md).

| ID | Status | Work | Acceptance |
|---|---|---|---|
| TH.1 | DONE | Promote Style → Theme; storage format 6 with ≤ 5 migration | `domain_tests` round-trip and migration; call sites on new names |
| TH.2 | DONE | `currentThemeId`, sparse slide customizations, full-chain resolution, undo | `domain_tests` precedence matrix per property |
| TH.3 | DONE | Compiler/controller follow the chain | `scene_tests`, `document_present_tests`, `frontend_journey_tests` re-skin to acknowledged pixels |
| TH.4 | DONE | More ⋯ menu and Theme Editor mode with live sample preview | `workspace_ui_tests`, `ui_loads`; browsing never writes to the worker |
| TH.5 | DONE | Apply/restore surfaces and Edit customization badges | `workspace_ui_tests` + controller regressions; bulk ops undo once — header selector sets the VIEWED show's default, cue and library right-clicks theme and restore, Edit fields write per-slide customizations with per-field revert badges and never restyle the shared theme |
| TH.6 | DONE | Captures and documentation | Three-width captures themed and customized; architecture.md updated — captures carry the Loud show default, the slide wearing House and exactly two live badges (the focus-out-is-not-an-edit guard this check found ships with it); a Themes section in architecture.md and the plan record the model, chain, surfaces and Qt 6.12 shapes |
| TH.7 | DONE | Themes carry the lyric-box placement (amendment) | `domain_tests` placement chain matrix (customization > current > slide > show > authored), validation and storage round-trip/refusals on format 6; `scene_tests`: the themed rectangle drives the FIRST TEXT item only — extra boxes and images keep theirs |
| TH.8 | DONE | Placement surfaces: Theme Editor canvas, Edit routing | `workspace_ui_tests`: theme-editor drag commits one placement (one undo removes the whole gesture), typed fields write it as one unit and refused values snap back, No placement clears it with colours left alone; Edit drag/typing on a themed slide customizes (THEME PLACES ↔ THIS SLIDE label, revert returns the whole rectangle to the theme), extra boxes keep authored edits untouched; the capture pair shows House's placement on the Edit canvas and the Theme Editor fields holding its numbers; architecture and the plan record the routing, and the re-shown-page sync this capture check caught ships with it |
| TH.9 | DONE | Embedded Default theme; every primary-box move is customizable (amendment 2026-11-02) | `domain_tests`: Default (id `default`) seeded first, re-ensured after load, chain's last layer, `removeTheme` refuses while rename/edit stay allowed; `document_controller_tests`: a geometry write on the PRIMARY box records a placement customization even with the chain silent (the authored rect stays untouched in the saved file), rows carry the APPLIED rectangle, extra boxes keep authored writes; `workspace_ui_tests`: Default leads the header menu and editor list with Delete disabled and a one-theme floor beneath it; captures inspected with Default as row 0; the amendment and the superseded TH.8 authored-write rule are recorded in the plan and architecture |

### Bible import — online passages into today's schedule (BI)

Planned 2026-11-02; depends on the Themes model and placement (TH.1–TH.8).
[Detailed access record, model and acceptance ledger](docs/ui/bible-import-plan.md).
Import only the passage the operator requests from Bible Gateway; never download
or install a full Bible, build a local scripture library or prefetch chapters.
References: [Bible Gateway passage and version example](https://www.biblegateway.com/passage/?search=1%20Corinthians%201&version=NIV)
and [ProPresenter Bible workflow](https://support.renewedvision.com/hc/en-us/articles/360041347594-Using-Bibles-in-ProPresenter).
Use the latter for passage selection, theme preview and playlist insertion only;
OpenShowQt offers one destination action: **Add to today's schedule**.

**Operator flow and connection:** open Bible import, choose a translation
(for example NIV), enter a chapter or verse range, fetch, select the required
verses, choose a theme and preview the generated slides before adding them.
Show **Online Bible lookup — internet connection required** in the importer.
Report connection loss, timeouts, unavailable translations and invalid passages
clearly, with retry; never substitute another translation or reuse an old result.
Fetching, selecting and previewing never change the schedule or audience output.
Require today's schedule as the explicit target; if none exists, require its
creation/selection before adding, and invalidate an add if that target changes.

**Extraction and lifetime:** use the HTTPS passage page with encoded `search`
and `version` parameters, fetching only the requested passage. Parse HTML
structurally into ordered verses with their references; decode entities and
normalize whitespace, excluding navigation, adverts, headings, footnotes,
cross-reference markers and study material. The result is plain text rather
than HTML; preserve Unicode punctuation and letters rather than losing them
through forced ASCII conversion. Retain translation identity and its required
copyright/attribution separately for the schedule item and slide formatting.
Keep fetched HTML and uncommitted text in bounded memory only; discard on new
lookup, cancel or close. No HTTP disk cache, temporary passage files, lookup
history, response-body logging or persisted importer previews.

**Schedule ownership:** only the selected verses and generated slides, reference,
translation and attribution become a schedule-owned presentation, in one undoable
add. Do not create a reusable Library show or expose Save to Library, Copy to
Presentation or direct-to-live actions. Schedule save/recovery may persist that
item as part of the service document; this is its sole durable content owner,
not a separate Bible cache. Imported slides remain presentable offline; further
lookups require a connection. Removing the item must not leave a Library copy.

**Theme-sized pagination:** use the resolved theme's primary text-box placement,
font family/size, line spacing and output canvas, reserving room for references
and attribution. Measure using the worker's existing SkParagraph/ICU layout;
do not estimate by character count or introduce a second text renderer. Pack
complete verses where they fit; split a long verse at sentence/word boundaries
across continuation slides, preserving order and verse identity. Never clip,
drop or repeat words, split inside a Unicode character, or silently shrink the
chosen font. Preview all slides; theme/size changes in the importer regenerate
pagination. An unusable text area blocks Add with an explanation. Later theme
changes to an imported item require explicit re-pagination and preview in Edit,
using its schedule-owned text without another network request.

| ID | Status | Work | Acceptance |
|---|---|---|---|
| BI.1 | DONE | Confirm provider access/translation attribution requirements; isolate passage/version discovery and HTML parser | Access approach recorded in [the plan](docs/ui/bible-import-plan.md) before implementation (passage-only HTTPS page fetch, robots-allowed path with pacing, no corpus, attribution carried; live markup facts recorded the same day); `bible_import_tests` on synthetic fixtures: live-shape parse with chapter/verse boundaries, entities and Unicode preserved, headings/cross-references/navigation excluded, invalid-reference, blocked and changed-markup pages each refuse with their own sentence and import nothing, and a page answering a different translation is refused naming both |
| BI.2 | DONE | Asynchronous bounded HTTPS lookup and transient importer state | `bible_lookup_tests` on a scripted network manager: the importer opens on the internet notice, one in-flight request whose URL is HTTPS with the encoded search/version parameters, a transfer timeout, no cache save and AlwaysNetwork load (the disk-silence rule pinned at the request itself); success lands verses pre-selected with the licence text, offline reports the notice and a retry re-asks, a timed-out transfer says so, cancel retires the reply so it cannot write a passage afterwards, and a superseded request answers to nobody — the newer passage stands; an answer over the 6 MB cap is refused unparsed, an HTTP error answers with its number, an empty reference sends nothing, and moving the translation drops the now-stale verses; 59/59 desktop and 44/44 headless |
| BI.3 | DONE | Schedule-owned scripture item and undo/save/recovery lifecycle | Domain: `createScriptureShow` (blank reference or translation refused), `scheduleOwned` + `ScriptureProvenance{reference,translation,attribution}` stored as optional format-6 show members — Library-only files save as before, and a pre-6 file carrying either key or provenance on a non-schedule show is refused naming it (`domain_tests`, round-trip field-for-field); controller `addScripturePassage(playlist, reference, translation, attribution, slides)`: one edit lands show + slides + today's-schedule row as ONE undo/redo unit, a vanished target is refused naming itself, the Library list, the Library count and the drawer search never see the item, save/reopen keeps the selected words, provenance and exclusion, and removing the row leaves no copy anywhere (`document_controller_tests`); 59/59 desktop and 44/44 headless |
| BI.4 | DONE | Theme-aware measured pagination and slide generation | Protocol minor 14's `text.measure` (capability `text.measure.v1`) answers from the worker's own SkParagraph/ICU layout at scale 1.0 — `text_measure_tests` against a live worker: doubling the size grows the height, narrowing the box wraps to more lines, Unicode measures and wraps, a batch answers whole in order and every cap refusal names itself; `scripture_pagination_tests` on a scripted engine: concatenated slide text preserves the selected passage exactly after whitespace normalization for wide/narrow boxes, larger sizes, sentence and word splits and Unicode, no block is measured twice, the reference/attribution's wrapped height is reserved out of the box, and a zero room, a single unfitting word, a dead engine or a malformed answer each block with their own sentence; 61/61 desktop and 45/45 headless |
| BI.5 | DONE | Import UI: translation, passage/verse selection, theme preview and schedule-only Add | `bible_import_page_tests` on a live worker: the page opens on the online notice, a scripted lookup answers and the selection paginates by itself with the slides' words exactly the passage, a theme change re-paginates with NO second request and a narrow box means more slides never smaller words (a split verse's slides say so in their labels), clicking previews stays off air (`wordsOnAir` false throughout), Add lands exactly one schedule row with the verse labels in the slides' NOTES — and the consumed batch makes a repeated Add structurally impossible; one undo removes the whole item, one redo restores it, and the saved file carries labels in notes with none on the slide's words; cancel discards everything and writes nothing, no worker means no slides and an actionable sentence naming the missing renderer, an unreachable provider fails on the lookup's own line; `workspace_ui_tests` adds the More-menu entry opening the page off screen — notice first, Add asleep and naming the schedule as its only destination, no output asked for; 62/62 desktop and 45/45 headless |
| BI.6 | DONE | End-to-end acceptance and operator documentation | `bible_import_e2e_tests` walks the milestone's sentences once, on a live worker with HOME, cache, data and config sandboxed into the scratch tree: the connected (scripted-provider) import lands on today's schedule and saves asking the network for nothing beyond the one lookup; the disk is then read as a witness — no fetched-page bytes (`passage-text`, `publisher-info-bottom`, version classes) appear in ANY file under the sandbox, the deselected verse's words appear nowhere, and the kept words live only inside the service's own records; the provider then becomes unreachable and the next lookup fails on its own actionable line while the saved row stands unchanged; a fresh controller reopens the service from disk and EVERY saved slide plays back through the ordinary one-call go on the real output with the provider down and zero further requests; fixtures stay synthetic throughout — no live provider is contacted from the suite (the parser's live verification is recorded in the plan, not gated on the network); operator documentation in `docs/architecture.md` (the five-move journey and its rules) and the delivered-shape record in `docs/ui/bible-import-plan.md`; 63/63 desktop and 45/45 headless |

### Bible import — operator feedback fixes (BI-F)

The importer's first live use (2026-11-02) confirmed the lookup works end to
end and surfaced three follow-up issues: **Add never woke** after a live
lookup (the button stayed disabled, so the operator could not add), the
reference field offers **no help naming a book**, and the translation picker
opened with **no row selected** — the session's NIV default only showed up
after the operator picked it from the menu by hand. All BI's constraints
still hold: passage-only fetching, nothing cached, measured pagination, and
today's schedule as the page's only destination.

| ID | Status | Work | Acceptance |
|---|---|---|---|
| BIF.1 | DONE | Add must wake: diagnose and fix the live-lookup case that leaves Add asleep | The diagnosis, from a production-shaped fixture (31-verse chapter, live-length attribution, live worker, the page's re-pagination hammering across the in-flight batches): the reference and attribution were reserved at the WORDS' display size — at the theme's unset size (the renderer's ~96 px default) the licence paragraph reserved 720 px of the ~850 px box, so 31 verses ground out into 94 near-one-line slides and Add stayed asleep behind the squeeze. The reservation is now set in caption type (clamp(4% of the box height, 16..56 px, never above the words' size) — BIF.1 records it) and is still measured by the worker at exactly that size: the same fixture produces 19 multi-verse slides, Add wakes, and one Add lands the chapter with every slide on the schedule-owned show. No path sits on "measuring": a batch unanswered for 60 s retires with its own actionable sentence (a dead worker retires it the moment the connection drops — both fixed by fixture), measurement caches key on content AND size, and the "no verses are selected" hint yields to the failure's own sentence instead of overwriting it; 63/63 desktop and 45/45 headless |
| BIF.2 | DONE | The translation picker opens on the session's translation (the -1 selection) | The root cause is recorded: `currentIndex: indexOfValue(...)` is a function call the binding engine cannot track, so the index is never recomputed when the model lands and the popup opens unselected until a click. Fixed by deriving the selection through an expression that actually reads the session's property; opening the page shows NIV — New International Version selected, `setTranslation` from the session side moves the picker to its row, and a lookup issued with the untouched default sends a byte-identical URL to one whose NIV was chosen from the menu (`workspace_ui_tests` + `bible_lookup_tests`) |
| BIF.3 | DONE | Book list and reference autocomplete | A Qt-free table of the 66 books with their common abbreviations plus a suggester in `openshow::importer/bookindex`: matching is case-, whitespace- and ordinal-insensitive ("1 cor", "1cor", "i cor", "1st cor" → 1 Corinthians; "jn" → John; "phm" → Philemon; "song" → Song of Solomon), exact matches rank above mere prefixes and canon order decides within a tier, and a typed chapter/verse tail rides into the completion whole. The reference field's dropdown filters with every keystroke and Enter or a click finishes the book name; unmatched text shows no rows and looks up exactly as typed — the helper never gates and no book list ever crosses the network. Two measured popup truths shipped with it: a Popup sizes itself to its `contentItem`'s implicit size, so the ListView had to *become* the content item (a free child left the rows outside the popup's own 4-px click area), and Material's enter/exit fades keep `visible` true until the exit transition finishes — so the completer's popup nulls both transitions and retires the instant the completion is taken (`bible_import_tests` + `workspace_ui_tests`) |
| BIF.4 | DONE | Re-acceptance and docs | The end-to-end fixture now walks the production shape after the BI.6 journey: the completer answers "1 cor 1" with the whole-chapter completion, a fresh session's untouched default (NIV) is proven — from the captured request URL — to be exactly what goes out, the whole chapter adds as one item beside the first passage, the sandboxed disk gives the same zero-cache witness a second time, and a disk-only controller plays every chapter slide with the network counter frozen at three. The never-substitute rule proved itself on the way: a fixture body answering EXAMPLE to the NIV request parsed to zero verses, exactly as specified. `docs/architecture.md`'s operator account tells the completer and the picker default, and `docs/ui/bible-import-plan.md` carries the BI-F delivered-shape record; desktop 63/63 (serial) and headless 45/45 |

Task order: BIF.1 → BIF.2 → BIF.3 → BIF.4, one commit per leaf.

### Theme text contexts and shared text flow (TF)

The operator's paging note: the theme should decide how a box fills with
text, and it may hold more than one view — song slides at two display
lines, a bible reading at six — with the whole downloaded passage flowing
as one run of text across those slides; the future ASCII song import is
meant to reuse the same split-and-size engine. A **context** is a
typography-and-box override of a theme (size, line spacing, alignment,
inset or its own rectangle — never colours), stored per theme under a
name (`scripture` is this milestone's consumer), resolved by the one
existing appearance resolver so preview and output cannot diverge; **no
leaf stores a line count** — how many lines a slide carries is whatever
the worker's measurement fits in that context's box, the operator's two
and six being how the contexts measure out, not settings. The packing
core of `paginateScripture` is extracted into a generic text-flow engine
whose scripture adapter keeps its current suite unchanged. Full design in
[the plan](docs/ui/text-flow-plan.md).

| ID | Status | Work | Acceptance |
|---|---|---|---|
| TF.1 | DONE | Theme text contexts and per-slide `text_context` through storage and the resolver | `themes[].contexts` (typography and box overrides only — the shape has no line-count field to store) and a slide's `textContext` round-trip through format 7; unknown context names travel untouched and resolve as no layer at all; a context entry refuses any unknown key by name ("never a line count or a colour"), duplicate names, and box+inset together, and both keys in a format-6 file are refused as another era's meaning; `appearanceForSlide` answers customization > named context (first theme in chain carrying the name, colours never) > themes, with `box` placing directly and `inset` shrinking whichever box the slide would otherwise land on; measured pixels: the reading-size passage compiles and draws at the context's typography inside the context's box where the display size wrapped, the song slide of the same theme draws byte-identically before and after the context exists, and a save/load round-trip draws byte-identically (`domain_tests`, `scene_render_tests` CPU rasters) |
| TF.2 | DONE | The generic text-flow engine, scripture reduced to an adapter | `openshow::importer` gained `textflow`: the packing, sentence/word splitting, continuation labelling, height reservation (`measureReservation`) and the `MeasureFn` port (`AwaitingMeasurement`) live there with no content kind named; `paginateScripture` is now verses-as-blocks + a reference labeler + caption-type reservation arithmetic over it, and its BI.4 suite passes **unchanged** (same measure-call sequence, same refusal sentences); the engine input carries no line count anywhere — the same four stanzas in 20/40/80 px of box measure out 4/2/1 slides; engine tests (`textflow_tests`) cover stanza-shaped blocks under a stanza labeler, a block that fits alone never being probed as a fragment, split continuations carrying the same block id with `(cont.)`/`(continues)` edges and lossless word reassembly, and the single-word dead end refusing by name |
| TF.3 | DONE | Scripture import flows the whole passage through the engine under the theme's scripture context | BIF.1's live-shape chapter, measured on the real worker through two services whose manifests differ ONLY by the context: the plain 96 px look packs 19 slides, the theme's 32 px `scripture` lectern box packs **4** — multiple verses per slide because that is what the measurement fits, never a set split and never one verse per slide — and the identity holds (nothing clipped, added or repeated). The off-air style, the previewed scene and the added slides all resolve through the one chain: `scriptureBoxStyle` answers the context's size and box, the previewed slide compiles inside it, and every added slide names `scripture` into format 7, labels in notes and provenance on the show exactly as BI.5 keeps them; a theme without the context answers 96 px/1600 px exactly as today and its whole existing suite passes unchanged. (Uncovered on the way: the resolver dereferenced a NULL pointer-to-member for the colours a context can never speak — latent undefined behaviour the tests had been surviving by layout luck; now guarded and defined.) |
| TF.4 | TODO | Theme editor section for the reading context; re-acceptance and docs | The theme editor sets the scripture context's typography and box — there is no lines field, and the editor reports how many lines the box currently measures out as information only; the Bible page's plan sentence names the context used; the end-to-end fixture re-runs under an edited theme and the PGM slide matches the previewed flow with the provider down; `docs/architecture.md` and the delivered-shape record updated; full desktop and headless gates pass |

Task order: TF.1 → TF.2 → TF.3 → TF.4, one commit per leaf; BIF.1 lands
first regardless (it explains the live shape the engine must control, and
Add is blocked until it does), BIF.2/BIF.3 stay independent, and TF.4's
re-acceptance rides after BIF.4's.

### M7 — Productization, reliability and installable alpha

These release gates remain open regardless of historical milestone tags.

| ID | Status | Work | Acceptance |
|---|---|---|---|
| 7.1a | TODO | Worker supervision and bounded restart/shutdown; reset decoder, requests, timers, previews and acknowledged state per connection | Kill/disconnect/hang/restart tests preserve editable documents, clear stale UI imagery/state and start uniformly black; explicit presentation required to go live |
| 7.1b | TODO | Device/surface/swapchain failure classification and recovery; check all Vulkan/Skia results and bound waits | Fault-injected submit/acquire/device loss cannot busy-loop, wait forever or claim success; device/session rebuild has explicit safe output state |
| 7.1c | TODO | Stable display identity/hotplug handling, disabled output on destination removal and refreshed UI display inventory | Unplug/replug/reorder cannot silently move audience output onto another display; explicit destination selection re-enables it; projector Wayland/X11 checks recorded |
| 7.2 | TODO | Playback/rendering diagnostics and measurable resource budgets | Frame/decode/upload/readback timing, source clock, signed A/V drift, dropped/held frames, queue/cache bytes, failures and transport restart counters inspectable; diagnostics do not block realtime paths |
| 7.3 | TODO | Package application with reproducible dependency/build manifest and notices | Both executables/resources/fonts run on clean Fedora with no build-tree path; matching public Skia/ICU artifacts, Qt/FFmpeg and other notices/licences included |
| 7.4 | TODO | Full service rehearsal, target-machine performance and acceptance fixes | Complete acceptance suite below: one-hour mixed service without growth, 10-minute deadline and 30-minute A/V gates, missing/corrupt media and recovery, physical second-display acceptance on Wayland and X11; remaining fixes get focused tasks |

Checkpoint: `M7: deliver installable presentation alpha`.

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
