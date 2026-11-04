# OpenShowQt: implementation plan

Accepted 2026-10-21; condensed 2026-10-30; theme architecture reviewed 2026-11-03;
Qt control-set review and pre-FO work added 2026-11-04.
Current checkpoint and verification: [PROGRESS.md](PROGRESS.md).
Current code boundaries and open differences: [docs/architecture.md](docs/architecture.md).
Detailed completed-task evidence: [history](docs/history/README.md).
Post-M6 conformance review: [original findings](docs/reviews/M6-conformance-2026-10-30.md).
Architecture review: [after 6.22](docs/reviews/M6-post-6.22-review-2026-10-31.md); [6.23 closure evidence](docs/reviews/M6-6.23-verification-2026-10-31.md).
Theme/editor review and proposed contracts: [2026-11-03 review](docs/reviews/theme-editor-2026-11-03.md).

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
  measured theme-sized slides and a named scripture context (BI/TF delivered).
  View/editor follow-ups are TV and CS below. No installed Bible or passage cache.
- **Deferred:** FreeShow import/round-trip, stage/additional outputs,
  Vulkan display-plane output, effects beyond TS below, remote control, streaming,
  NDI/SDI, cloud services, Windows and macOS.
  Future output-profile/NDI/stage work is recorded separately in FO below;
  reserving its design does not enable another output in this release.

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
   The implementation runbook below is an intentional exception for unfinished
   TV/TS/TR/CS work: retain actionable steps until delivered, then move detailed
   implementation evidence to history and keep the completed ledger concise.

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
| 6.20e | DONE | Record M6 closure evidence and environment limits | [Architecture closure](docs/architecture.md#m6-closure-the-completion-rule-in-writing-task-620e); archived 6.20d gates |

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
| TF.4 | DONE | Theme editor section for the reading context; re-acceptance and docs | The Theme Editor's inspector carries a **Scripture Reading** section: the toggle creates the `scripture` context from the theme's current look and box, size/alignment/spacing/box fields commit through undoable verbs (empty field = silence falling through to the themes; a refused value snaps the fields back; remove refuses when there is nothing to remove), and the section states "the box holds ≈ N lines" via the worker's own `text.measure` — **there is no lines field anywhere**, and the honest no-worker sentence prints instead of a number. The resolver itself now reports which context layered (`ResolvedAppearance.contextName`), so the Bible page's plan sentence says "measured to fit in the theme's scripture context" in the resolver's own word and stays byte-identical when none answered. The e2e re-acceptance: context applied through the editor's verbs, the chapter looked up, Add, save, reopen — the schedule holds exactly the previewed slides, every saved scene compiles at the context's size inside its box, and the presented PGM picture is the previewed flow (`workspace_ui_tests` TF rows, `bible_import_e2e_tests` TF leg); `docs/architecture.md` gained the milestone's delivered-shape section and `docs/ui/text-flow-plan.md` its as-built close; gates 64/64 desktop serial, 46/46 headless |

Task order: TF.1 → TF.2 → TF.3 → TF.4, one commit per leaf; BIF.1 lands
first regardless (it explains the live shape the engine must control, and
Add is blocked until it does), BIF.2/BIF.3 stay independent, and TF.4's
re-acceptance rides after BIF.4's.

### Theme views and workspace follow-up (TV)

Reviewed 2026-11-03. Call the user-facing variants **theme views**: Songs uses
the base theme; Bible uses the existing `scripture` context, initially unchanged.
Use the workspace below the header for Themes, with view navigation left, a
large worker-rendered canvas and a scrollable inspector. The review records
current gaps, migration/precedence, proposed contracts, estimates and test matrix.
TH/TF completion remains historical evidence; every new leaf starts TODO.

| ID | Status | Work | Acceptance |
|---|---|---|---|
| TV.0 | DONE | Review domain, storage, editor, scripture flow, renderer and transitions against references | [Findings R1–R12, proposed contracts and baseline evidence](docs/reviews/theme-editor-2026-11-03.md); 22 focused checks pass with local IPC/display access; current editor capture and three-font stimulus inspected |
| TV.1 | DONE | Select Songs/Bible views and compile the selected view's complete off-air fixture | `themeSampleSceneJson(themeId, view)` and the new read-only `themeSampleAppearance(themeId, view)` share one fixture — a **copy of the live Documents** with the current theme cleared, the selected theme as a throwaway show's default and the sample slide naming the view's context — so the edited Default floor and every theme's contexts enter the chain as the audience walks them and a worn theme cannot out-resolve a browsed one; the Bible sample compiles the reading (reference, running text, Unicode) and the Songs sample the cue. `ThemeEditorPage` gained `selectedView` (`""`/`"scripture"`, `themeViewSongs`/`themeViewBible`), Songs/Bible inspector sections, an overlay and fields reading the compiled fixture's own rectangle, and gestures that commit to the view they started in (a Bible box write retires a stored `inset` in the same edit). Format 7 unchanged; `document_controller_tests` (views, wholesale context inheritance, unknown-name silence, browsing marks nothing), `workspace_ui_tests` (selection held, two records stay two, browsing draws no audience) and `preview_selection_tests` (`testViewSwitchFollowsPixels`: the newest ask's pixels replace the old view's, the audience stays flat) pass; gates 64/64 desktop serial, 35/35 headless |
| TV.2 | DONE | Full-workspace Themes layout, view thumbnails and reachable inspector | The operator docks and bin are HIDDEN for Themes — visibility only: `WorkspacePanels` keeps every stored width and drawer height, hidden items keep their geometry so the other pages still anchor by id, and leaving puts the arranged workspace back whole. The page spans the window under the header with bounded columns (220 nav / 300 inspector / positive centre), the inspector is a ScrollView whose first and last fields reach at 1000×680, and the canvas fits `min(available, height×16/9)` centred at all three sizes. The nav footer carries live Songs/Bible thumbnails through the existing preview transport (one batch of both sample scenes per selection, keys per view+theme, coalesced ask, standing down so the Show band's batch never competes). Themes-only emergency `Clear` in the header (restore-or-clearAll, output fullscreen torn down by the existing hide rule). Exiting cancels half-finished gestures and takes the keyboard with the page: a plain field release falls onto the inspector's ScrollView Pane and the window's deferred chain recomputation re-arms it, so the page releases every focus holder inside itself and re-acquires for the window root (with a `Qt.callLater` belt in the shell handover). `workspace_ui_tests` (hidden-but-stored docks, page spans, canvas fit at 1000×680/1380×800/1680×1020, inspector reach, tile clicks per size, typing→exit leaves no trapped field, batch/re-ask, LIVE thumbnail arrival) pass; captures `.themes-{1680,1380,1000}.png` and `.themes-bible-*.png` show no overflow; gates 64/64 desktop serial, 35/35 headless |
| TV.3 | TODO | Promote sparse scripture contexts to appearance views with versioned migration and provenance | After TS.1's shared style contract: allow independent view colours/effects, preserve first-context-wholesale precedence, inherit vs explicit off, unknown names, Default and customization/revert; old services keep pixels/IDs; malformed or old-format new keys refuse atomically (`domain_tests`, `scene_tests`, `document_controller_tests`) |
| TV.4a | TODO | Bible body/reference/attribution roles in domain, storage and scene compilation | Independently resolved typography/boxes compile to flat scene items, within item budgets; legacy primary/extra boxes unchanged and no captions silently added on migration; one-undo/round-trip, role validation and geometry assertions (`domain_tests`, `scene_tests`, `document_controller_tests`) |
| TV.4b | TODO | Measure and generate the Bible roles through preview, Add and offline playback | Actual body/caption regions match reservation and rendered ink; long reference/attribution and impossible layouts tested, exact selected words conserved; one undo, save/reopen and provider-free playback (`scripture_pagination_tests`, `text_measure_tests`, `bible_import_page_tests`, `bible_import_e2e_tests`) |
| TV.5 | TODO | Retire stale view measurements/previews and stale Bible plans across mode changes | Returning after theme edits remeasures in-memory words; Add requires current style/worker/selection/target generation; delayed replies cannot re-enable Add or display wrong-view pixels; committed passages retain authored boundaries; connected/disconnected and reachability journeys cover real interactions |

### Text styling and effects (TS)

Keep SkParagraph as the only layout/measurement engine. Author font weight/style,
stroke, shadow and fit policy through the domain and resolved scenes; preserve
old pixels by default. [Units, inheritance, font fixtures and renderer constraints](docs/reviews/theme-editor-2026-11-03.md#typography-and-effects-contract).

| ID | Status | Work | Acceptance |
|---|---|---|---|
| TS.1 | DONE | Shared typography/effects/fit records, undo, storage and validated scene/protocol contract | Shared `TextStroke`/`TextShadow` records and the whole-set `ThemeTextStyle` patch in the Qt-free domain, a `setThemeTextStyle` verb, and a resolver walking customization → first CARRYING theme → the contract's concrete defaults (400/normal/shrink/12/effects disabled — old documents keep their exact pixels); contexts stay silent until TV.3. Service format 8 (`kServiceFormatTextStyle`) writes the six theme and customization keys only when set, refuses hand-bumped pre-8 keys naming the era, and re-vets a loaded theme's style block with the domain's one `validThemeTextStyle` sentence — what loads is what a verb would accept. Scene format 4 always writes the resolved style block on every text item (never defaulted by absence), effect records cross WHOLE with packed colours and `enabled:false` honoured as the explicit refusal, and legacy scene 3 reads onto the defaults while scenes 1–2 still refuse and style keys on an image item are refused by name. Protocol minor 15 defines `scene.text-style.v1` but deliberately does NOT advertise it until TS.2 draws the fields; `WorkerController` gates every scene send (go, present, preview, thumbnails) and refuses a deviating scene naming the capability with not one byte sent. The controller's `setThemeTextStyle`/`customizeCurrentSlide` refuse unknown keys, string weights and half records rather than coerce; `duplicateTheme` carries placement, contexts and style (the dangling-pointer read of the duplicated theme fixed); `createThemeFromCurrent` snapshots the chain's concrete answers; `DocumentViews` rows carry `...Set` flags with the effects as maps. `domain_tests` (verb bands every-field-naming, resolver precedence and explicit-off, byte-identical styled round-trip, silence writes no keys, era refusals including fragment/stray effect records), `scene_tests` (scene-4 always-written keys, legacy-3 equality onto defaults, band and whole-record refusals, round-trips of the widest legal and disabled records), `protocol_tests` (the capability's ABSENCE pinned for TS.2 to update consciously), `document_controller_tests` (verb strictness, one-edit undo/redo, customize/revert badges, duplication and from-slide carry, reload) and `worker_controller_tests` (gate closed/open/re-handshake) pass; gates 64/64 desktop serial, 35/35 headless |
| TS.2 | DONE | Render font faces, stroke and shadow through public Skia APIs | Weight and slant enter shaping through public `TextStyle::setFontStyle`, so the FontCollection resolves the bold/italic FACE; the shadow rides `TextStyle::addShadow`, and the pinned fork paints background → shadow → blobs → decorations from a copy of the SHAPED run — shadow and fill share wrap and position by construction, opacity folded into the shadow colour's alpha once. The stroke cannot ride a style (one style paints one pass), so `GroupAlphaPainter` draws outline-then-fill from THE same blob per item — no second differently-styled paragraph is shaped anywhere. Probing the pinned Skia measured the blur mask-filter sigma to be DEVICE-space (the same sigma spreads the same pixels at canvas scales 1.0 and 0.5) while stroke widths ride the CTM, so the painter multiplies the authored sigma by the canvas scale (memoised) and passes the authored width straight through. The frame's fade alpha rides the one painter and multiplies each effect's own alpha exactly once — no `saveLayer`, no intermediate target — and a full-alpha effect-free stack keeps the pre-TS.2 fast path byte-for-byte. `scene.text-style.v1` is now ADVERTISED in the hello (18 capabilities; minor stays 15, which reserved it) and the protocol_tests tripwire consciously flipped to presence. Raster fixtures (`testTextStyleFacesAndEffectsRender`): loud-but-disabled effects are byte-identical to none; bold and italic share the regular wrap and band centres with measured +32% bold ink; a 28-unit stroke grows the ink by half its scaled width on every side (≈7 device px), paints a red band beyond the fill's own edge, and survives under the fill (≥90% of the white pixels); the hard cyan shadow lands exactly at the fill's right edge + offset×scale with unchanged top/bottom, blur spreads its ink and dilutes the peak, opacity 0.5 composites half-bright; the 50% text fade measured on single-painter regions gives fill/stroke/shadow peak ratios 0.50/0.50/0.50 with geometry kept, and the wire round-trip renders byte-identically. The GPU live path (`live_fade_tests`) settles a styled scene carrying 5892 stroke-red / 3762 shadow-cyan / 10171 white preview pixels, lands clean to black, and passes each effect's masked mean through the middle of its ramp across four separate fade frames — it rides the text alpha rather than cliffering or double-folding. Gates: `scene_render_tests`, `unicode_text_tests`, `scene_frame_tests`, `live_fade_tests`, `skia_public_api_guard` and `render_layer_guard` all pass inside desktop 64/64 serial, headless 35/35, `git diff --check` clean; the `worker_controller_tests` gate scenario still refuses non-default scenes toward a capability-lacking peer |
| TS.3 | DONE | Shared ink bounds, effect-aware fit/measurement and complete style cache keys | `layout()` carries two extents on every laid-out item: `ink` (the fill's line-metrics box grown half a pen each side by an enabled stroke and unioned with the offset copy grown by the finite ~3.1σ blur support — the pixel truth) and `fillInk` (the pre-effect box the WORD verdict judges); the new `fitMode:"fixed"` branch lays out once at the authored size and answers instead of silently shrinking, while the legacy shrink loop is untouched. Protocol minor 16 defines `text.measure.v2` (`kCapabilityTextMeasureV2`, hello now 19): blocks may carry the resolved style (`fontWeight`/`fontStyle`/`fitMode`/`minFontSize` and whole packed-colour effect records, every band and closed record refused by name, per-key `offsetX`/`offsetY` sentences), and EVERY reply — v1-shaped asks included — grows `fits` plus `bounds{empty,left,top,right,bottom}`. The two divide honestly: `fits` judges the FILL inside the box (the one question repagination answers — SkParagraph char-breaks a too-wide word, so words move and the fill stays in; the height leg is where fits=false arises), while `bounds` carries where any pixel may land, so an enabled effect's spill — which no repagination can shorten — is reported, never decided (folding it into the verdict would refuse every page of a shadowed theme). The frontend gate is the honest half: `WorkerController` reads the hello flag (`styledMeasureSupported()`), packs hex→int and refuses a style-DEVIATING ask toward a pre-16 peer naming `text.measure.v2` with nothing sent (pre-16 workers silently ignore unknown keys — a styled ask would be answered in the wrong face), while disabled-loud records and spelled defaults travel; reply rows parse optional `fits`/`bounds` additively. The flow (`textflow`) forwards the whole `FlowStyle` verbatim on every ask and answers `Answer{height,fits}`: the criterion is `height <= available && fits != 0`, with fits=-1 (pre-v2) leaving the height as the whole judgment — the pre-TS.3 floor honoured, never a fabricated yes; `validateFlowStyle` vets weight/slant/fit/floor/effect bands (NaN σ included) before the port is touched. `measureCacheKey` — one canonical length-prefixed serializer over content, box AND the whole style — is shared by the engine's per-run cache and ScriptureImport's across-run cache (a theme changed mid-session is a different question; the correlation key is not part of it), capped at `kMaxMeasureCacheEntries` 4096 (stop inserting / clear-on-full); importer links the Qt-free `openshow_domain` so `FlowStyle`/`MeasureRequest` carry the same `TextStroke`/`TextShadow` records — "identical inputs" is a type-level claim. `scriptureBoxStyle` reads the full style off the compiled fixture's first text item, the very item the slide draws. Evidence: raster containment at the 0.5 fixture scale for fill/stroke/shadow/northwest-offset/both (every drawn pixel inside `ink` mapped through the device scale, growth arithmetic ±half-pen and offset+3.0..3.1σ); IPC rows prove defaults-spelled bit-identical measurement, weight-800 measuring wider than 100, stroke ±half-pen bounds, shadow union bounds with honest reach both directions, spill-still-fits, char-break lineCount, height-leg fits=false and 15 by-name refusals; flow fixtures pin the style riding every ask verbatim, verdict-No refusing by its named dead end, the no-verdict floor packing unchanged, the cache-key field matrix and length-prefix honesty; `worker_controller_tests` S8c choreographs gate closed (styled refused by name, zero `text.measure` frames on the socket, defaults answered without a verdict) and open (records arrive whole with colour packed, verdict+bounds parse back), reconnect included. Gates: desktop 64/64 serial, headless 35/35, `git diff --check` clean |
| TS.4 | DONE | Installed font/face picker and selected-view typography/effect controls | `font.list` (protocol minor **17**, capability `font.list.v1`, hello now 20) is answered by `SceneTextEngine::listFonts()` — the renderer's OWN font manager, which is the only honest answerer of "which families can this shaper resolve", since it falls back silently from names it cannot match. Metadata only, never font bytes: `{family, faces[{weight, italic}]}` sorted by name with faces ascending; params are nothing or `{from}` (every other key and a negative/non-integer `from` refused by name); the page is capped at `kMaxFontListPageFamilies` 128 so one reply stays far inside the 1 MiB frame however many long PostScript names fontconfig knows, and reports `{families, from, total, truncated}`. The scan runs ONCE per worker lifetime under its own mutex (one fontconfig walk, on the RPC dispatch thread — never a presentation or audio callback), every later ask copies the cache, and a `from` past the end answers an EMPTY page rather than an error (a list shrinking under a mid-walk asker is legal; re-reading from 0 is the asker's repair, not the protocol's). A session with no engine returns the worker's own error sentence instead of zero families — "this box has no fonts" is not what happened. `WorkerController` owns the walk per CONNECTION: `fontListSupported`/`fontListState` (idle→loading→ready, `unsupported` for a hello that names no font.list, `unavailable` for a failed ask)/`fontFamilies`/`fontFaces` + `refreshFontFamilies()`, with retirement keyed by REQUEST ID and a continuity check on the echoed `from`, so a page from a dead session or a superseded walk is dropped silently and two sessions' pages can never splice one list. The one-field merge verb `setThemeTextStyleField(themeId, field, value)` sits beside TS.1's whole-set verb rather than growing `editTheme`: 13 leaf fields plus `"stroke"`/`"shadow"` for the inherit button's whole-record clear; null clears a scalar to inherit or names a whole-record clear and is REFUSED on an effect member (an effect means one thing whole); merging a member into a theme carrying no such record materialises the contract's defaults — the very pixels the audience draws today — so enabling an outline yields `{true, #000000, 2}`, never a fragment or another theme's leftover; exactly one `edit()` per call, and the merged block passes the domain's one `validThemeTextStyle` sentence, so a refusal changes nothing and costs no undo. The TV.2 rail's Songs section carries weight/slant/fit, minimum size, stroke enable/colour/width and shadow enable/colour/opacity/offsetX/offsetY/blur, each with its own "Off", plus the family field and a searchable Browse popup with a live sample; each control shows the theme's own value when it carries one and the RESOLVED answer otherwise, read through `themeSampleAppearance`'s new resolved-style extension (the compiled sample's first text item plus `ResolvedAppearance`) with a badge naming which — and because TV.3 has not promoted the reading context to a view of its own, these route to the base theme, so the scripture context carries no style fields until then. Gesture discipline is the tested half: sliders commit ON RELEASE (`onPressedChanged`, Qt 6.12 `Slider` has no `onReleased`) and write nothing while held, a field committed unchanged writes nothing AND costs no undo, a refused write re-reads the record and snaps the control back, the typed family works with no worker at all, and the note refuses to invent a verdict ("this worker cannot list its fonts" / "checking…" / "not installed on the audience's machine — it will fall back") with MISSING judged only from a COMPLETED worker list and the stored name NEVER rewritten; Browse sleeps until a worker that can list is ready, so no click opens an empty picker. The tests also found a real bug: the rail's scroll verbs asked `inspectorContent.parent` for `contentY` — the Flickable's own content item, which cannot scroll — so they now walk parents for a node carrying both `contentY` and `contentHeight`, compute the end themselves (a Flickable does not clamp a handed `contentY`; past the last row is blank paper) and add `scrollInspectorTo(name)`, because reachability is a promise about every control, not only the ends. Evidence: `text_measure_tests` (real fontconfig — sorted page, ascending faces, `truncated` exactly at the cap, a repeated ask byte-identical to the first = one cached scan, page 2 continuing name for name, from-past-end empty, refusals by name with the session living on); `worker_controller_tests::scenarioFontListWalk` (blind peer says `unsupported` with ZERO `font.list` frames on the socket; a two-per-reply fake pages a five-family scan through `from` 0,2,4; disconnect mid-walk retires the state and drops the late pages; a superseded refresh retires the older walk by request id; an error reply lands `unavailable` with the worker's sentence); `document_controller_tests` (every field band and refusal named, defaults materialising around one member, whole-record clears, one undo per gesture, disk round-trip, and preview-vs-saved agreement: the sample scene's first text item carries the same `stroke` and `fontWeight` as the compiled audience slide); `workspace_ui_tests` (combo writes its own field and neighbours keep silence, undo repaints the control from the record, unchanged commits cost no undo, an off-band minimum size snaps back, the outline toggle materialises `{true, #000000, 2}`, un-ticking keeps `enabled: false`, "Off" clears and sleeps, a held blur handle writes nothing while held / commits once on release / one undo backs the whole drag, a typed family saves with the "cannot list" note while Browse sleeps and opens nothing; the geometry loop now checks the true last control `themeEditorShadowBlur` at all three window sizes and the keyboard leg scrolls by name). Gates: desktop 64/64 serial, headless 35/35, `git diff --check` clean |
| TS.5 | DONE | Reproducible font/effect raster and performance acceptance | The corpus is `tests/fixtures/fonts/manifest.json`: google/fonts revision `2fe1e8c6b137fb13041a7cfca6fdcd8a910ea4dc` — the revision the review recorded — with `Inter[opsz,wght]`, `Lora[wght]` and `Oswald[wght]` **byte-for-byte the hashes the review's table carries** (the suite takes the review as argv[2] and asserts the agreement BEFORE any skip decision, so a drifted manifest fails even on a machine that has no corpus), plus `NotoSans[wdth,wght]` as the fixed fallback face the review asked TS.5 to add, and each family's own `OFL.txt` pinned too because the four copyright lines differ. The fonts are NOT vendored (3.3 MB of third-party binaries for a handful of fixtures is a packaging-policy question, not a test decision), so `tools/setup-font-fixtures.sh` prepares a developer cache BY HAND — `prepare --download` or `--from DIR` (which finds either the flat or the upstream layout), plus `verify`/`status` — installing nothing into the system, hash-checking every file it places and deleting a bad one; `$OPENSHOW_FONT_FIXTURE_DIR` else `${XDG_CACHE_HOME:-$HOME/.cache}/openshow/font-fixtures` is the one directory both the script and the test read. **ctest never downloads.** `font_fixture_tests` re-hashes all eight files itself before it renders a pixel (the sha256 is implemented in the suite, since it is Qt-free, and is self-tested against published vectors including the 56-byte length where padding spills a block): ABSENT is a visible SKIP — exit 77, which ctest prints as `Skipped`, its own output naming the command to run — while CORRUPT is a FAILURE naming the file and both hashes, because "you have not prepared the corpus" and "your corpus is not the corpus we pinned" are the difference between an honest skip and measuring a different face. Isolation is real, not aspirational: a private `fonts.conf` listing only the corpus directory with its own `cachedir` and **no `/etc/fonts` include** is set before the engine exists, `XDG_CACHE_HOME` moves too, and `fixtureCorpusIsolation` asserts the engine's OWN `listFonts()` scan names exactly the four pinned families and that `OpenShow No Such Family 47` resolves to nothing — resolution before pixels, because every claim below is about a named face. Fixtures, over all four families: body/caption line heights hold the 64/28 ratio within 5%; four files are four faces (pairwise coverage counts differ and >2000 pixels differ, with the condensed face >10% narrower than the sans — bounding-box WIDTH is explicitly NOT the discriminator, Inter and Lora coinciding to the pixel here); weight 100→900 resolves a face (>20% more ink) WITHOUT moving or re-wrapping a line (every row-band centre within 1 device px); the widest legal stroke (32) grows each edge by half its pen and a sigma-64 shadow offset −40/−30 reaches 20 px left and 15 px above the fill's own box, with every pixel of both effects together inside TS.3's `ink` at the 0.5 scale and `fillInk` **exactly unmoved** (an outline must not be able to move a `fits` verdict); shrink fits its box while `fixed` keeps the size and visibly overflows it (40 px vs 344 px in a 130 px box); the deliberate missing family falls back to a face that is IN the corpus, drawing byte-identically to that family named directly and unlike the other three; the per-family glyph matrix is printed and claimed only where the face has the glyph, with scripts nothing covers arriving as the placeholder box — the corpus's boundary, stated, not a product promise. The fade fixture needed the right ruler twice over, and both lessons are in the code: an absolute background threshold reads a half-bright antialiased edge as a word that shrank by 2 px, so placement is compared at each frame's OWN peak (the boxes are then identical), and each pass is separated by HUE rather than distance-to-authored-colour, because a blurred shadow blended with the backdrop has no pixel near `#00FFFF`; fill/stroke/shadow then attenuate to median ratios 0.581/0.492/0.502. Costs are the record, printed cold-by-ordering (this fixture runs before anything touches the engine): whole-corpus scan 0.4 ms and 0.00 ms cached, first shape ≤0.9 ms per family and ≤0.01 ms steady, 12 scene builds in 1.7 ms with the layout cache at exactly 4 resident / fifth evicted / fifty draws hitting one retained layout — occupancy finite after churn and no re-shaping on a steady draw, observed by pointer identity rather than inferred from a stopwatch. No product change was needed: TS.1–TS.4 already ship every surface these fixtures exercise, and no format was claimed (nothing here is persisted meaning). Evidence: `font_fixture_tests` (desktop 65/65 and headless 36/36 serial), plus `docs/fixtures/fonts.md` for the record and the skip report and `tests/fixtures/fonts/README.md` for the licence and policy note |

### Configurable slide transitions (TR)

Store a service default plus optional incoming theme-view override; send resolved
options with each Go intent through the existing prepare/present/store path.
Expose Cut/Crossfade and separate foreground/background durations; preserve
500/800 ms defaults. [Timing, video limits, clear/restore and compatibility](docs/reviews/theme-editor-2026-11-03.md#transition-contract).

| ID | Status | Work | Acceptance |
|---|---|---|---|
| TR.1 | DONE | Validated/persisted transition options and intent-bound RPC/store handoff | The record is `domain::TransitionOptions{kind, textDurationMs, mediaDurationMs}` — Qt-free, atomic, `operator==` — and there is exactly ONE sentence about what it may say, `Documents::validTransitionOptions`, quoted by the three verbs, by the loader's era-gated read and by the worker's parse of `scene.present`, so `10001` costs the same words at every door and no door can accept what another would refuse (each refusal names both the operator's word and the wire key: a settings page quotes this to a person and a socket to whoever wrote the payload). The band is whole milliseconds 0..10000 with the contract's 500 ms words / 800 ms media, and `defaults()` / `isDefault()` / `durationsAreDefault()` split the two questions the wire actually asks — `durationsAreDefault()` is the gate's question because `"cut"` is a spelling `scene.present` has carried since task 3.2, so a Cut cue travels to ANY worker at full fidelity while a NUMBER the peer has never met is the only half it could silently ignore. Seats: one CONCRETE service default (no "unset service default" for a resolver to invent — the 500/800 every present performed before this setting existed IS its value) plus optional whole-record overrides on `Theme` (the Songs view) and `TextContext` (the Bible view), inheritance following the chain the typography already walks — named context first (the view that set the slide is the nearer answer about how that view moves), then the FIRST theme in chain order (an explicit Cut stops the walk exactly like a fade: a decision, not an absence), then the service; a slide or show the model cannot name is not a refusal and not a guess, because the audience is owed a reveal either way. Storage is format **9** (`kServiceFormatTransition`): the root record is written always and REQUIRED (a 9-file with no `"transition"` is not a package this build produced, and guessing which fade an unnamed key meant is how a document starts lying about its own output), the theme's and context's are optional like placement, and the key is era-refused at all three sites — root, theme and context — in a pre-9 file, where it is another build's meaning of a key this writer never emitted; a silent document writes EXACTLY ONE `"transition"` (the root's), so old documents keep their shape, and re-saving is byte-identical. Resolution is captured ONCE when Go is issued: `ScenePrepareAsk.transition` is a value copy stamped by `armPrepare` through `Outbound::resolveTransition` (the documents answer, injected by `main.cpp`; `WorkerController` never learns what a Theme is), and `handlePrepareAck` puts THAT record on `scene.present` and consults nothing mutable — by the ack, "the settings" are whatever the operator has moved on to, and the cue they already pressed is owed the fade it was promised. Down the socket the whole record rides the change (`SceneStore::Snapshot.transition`; a repeat ask for the scene already desired changes nothing and CANNOT retime the reveal already owed, which is the R13 rule extended rather than a new one), `OutputSession::presentScene(prepared, revisionOut, const TransitionOptions&)` takes the record with NO default argument — a legacy entry point says so by handing over `defaults()` in the open, because a present that quietly invented one is how a queued reveal ends up timed by whatever the settings screen held when the frame was built — and the landed handoff narrows to the renderer's vocabulary at ONE named seam, `sceneTransitionKind()` in `render/Layer.h`, whose comment states why: this build's crossfade still runs on the library's own 0.5 / 0.8 s constants, so the pixels would not keep a promise the wire made. Hence protocol minor **18** with `scene.transition-options.v1` DEFINED AND DELIBERATELY WITHHELD from the hello list — the claim is earned by the renderer, not by the parser, exactly as `scene.text-style.v1` waited for the stroke that actually drew (TR.2 adds the name and flips `protocol_tests`' tripwire). `scene.present`'s two optional fields are vetted before the store is touched and whatever the kind says (a cut-shaped exemption would let a malformed number land on a scene that happens to cut): fractional, string, boolean, negative, >10000 and no-millisecond-count numbers each refuse as invalid params naming their key, and none of them moves the audience or queues anything. Toward a peer that does not honour durations the frontend's gate is a DOWNGRADE, not a refusal — a deliberate departure from the TS.1 style gate, because an operator who pressed Go is owed the next cue: the two keys are left off the payload (so an old worker receives exactly the legacy-shaped command it has always understood) and the status line says the cue moves on the worker's own 500 ms / 800 ms fade instead of the pair asked for. Silence is honoured in both directions: an unconfigured document sends byte-for-byte the pre-TR.1 command and says nothing. Evidence: `domain_tests` (bands and both tracks named, the walk seat by seat including inherit-is-not-a-default and silence-is-not-a-zero-length-fade, all three era refusals, the whole-set write unable to smuggle past the verb's sentence, byte-identical re-save, "exactly one transition key" for a silent service); `scene_store_tests` (100/200 rides the snapshot, a 900/1000 repeat changes nothing and cannot retime, a newer scene keeps its own lengths); `protocol_tests` (minor 18, the name defined with its spelling pinned and ABSENT from the hello); `rpc_ipc_tests`/`worker_rpc_lifecycle` (nine malformed shapes each refused naming their key, including two CARRYING `transition:"cut"`, the band's ends and a cut-with-record accepted, and output.status afterwards proving nothing moved and nothing was pending); `worker_controller_tests::scenarioTransitionIntent` (a peer holding A's prepare ack 400 ms while the resolver moves 100/200 → 900/1000, with the present's params logged VERBATIM: the wire says 100/200; a capability-less peer gets the cue anyway, with neither key on the socket and the downgrade in the status line naming both clocks; no resolver and no capability sends the pre-TR.1 payload in silence); `document_controller_tests` (three verbs, undo/redo of the service default, refusals naming `curve`/`whole or absent`/a wrong theme or context and changing nothing, the context override outranking the theme wearing it through a real imported passage, the Go-time read from a compiled payload, a duplicate's copy resolving to the copied record, disk round-trip). Gates: desktop 65/65 serial, headless 36/36, `git diff --check` clean |
| TR.2 | DONE | Renderer uses landed per-command durations and documented video behaviour | The record is drawn, so the name is earned: `SceneRenderer::setActiveScene(ActiveScene, const domain::TransitionOptions&)` (NO default argument) takes the cue's whole answer and blends on `textDurationMs` / `mediaDurationMs`, `OutputSession`'s landed handoff passes `snapshot.transition` across undivided, and TR.1's narrowing seam — `SceneTransitionKind` and `sceneTransitionKind()` in `render/Layer.h` — is RETIRED, its comment now saying why there is nothing left to translate into (a record narrowed on the way is a promise dropped on the way). `kTextFadeSeconds` / `kMediaFadeSeconds` are gone: `SceneTransition` carries `fTextSeconds` / `fMediaSeconds` converted ONCE when the cue lands, because `captureFrame` runs at the panel's rate and a divide there would be paid sixty times a second for two numbers that cannot change while the fade runs — a record belongs to its cue, so nothing after it is stamped can retime it. The two entry points with no command behind them (the development harness, the snapshot CLI) quote the DOMAIN's `kDefaultTransitionTextMs` / `kDefaultTransitionMediaMs` rather than a second pair of constants, so there is no number here for a later leaf to forget to move. `fadeBlend` tests `duration <= 0` BEFORE `elapsed <= 0` and the order is the acceptance: a 0 ms track is an answer the operator gave, the fade's first captured frame has elapsed 0.0, and an elapsed-first test would show the outgoing scene to someone who asked for nothing to fade (a 500/0 cue's words arriving a frame late, a 0/0 fade taking a frame to do a cut's job). Retirement is unchanged and still the present's job — the outgoing scene goes only when the SETTLED frame reaches the panel, and a preview draw still cannot move the clock. The two frontend rules the contract names are wired and read off the socket: every explicit clear presents `transition:"cut"` (a clear is not a cue — taking something off the output whatever the cleared slide's theme says about how it ARRIVES), and each clear captures the record its retained content is owed (`fRestoreTransition`, resolved from THAT payload at the moment of retention beside `fRestoreJson`, retired with it), so `restoreOutput()` moves with 100/200 when the setting has since moved to 900/1000. Rapid Go keeps the existing rule: the in-flight fade is replaced from the scene that was coming up, restarted at its own first frame on the NEW cue's lengths, never composited into a new render target. Video is stated rather than sold: the background layer still takes the media cut whenever video is involved in either direction — a crossfade there would composite two stills over decoders that have not produced frames — so no media duration buys a dissolve between clips, while the WORDS keep their crossfade and a cue that keeps the same video background keeps playing it continuously because the session restarts nothing when the identity is unchanged. `scene.transition-options.v1` now rides the hello (21 capabilities), `protocol.h`'s minor-18 prose rewritten to what earns it, and the downgrade path survives as what it always was for a pre-18 worker or a capability-less scripted peer. No version was claimed: the wire is exactly TR.1's and only the pixels changed. Evidence: `scene_frame_tests::testDurationsComeFromTheCue` (0/0 is the settled picture on its very first frame yet still owes the present that retires it; 0/800 has the outgoing word GONE from a frame whose background is still crossing; 500/0 lands the colour with the outgoing word still on it — the two tracks are not one; 100/200 reads as half the words and a quarter of the background at 50 ms, in agreement between panel and preview, over at 200 ms not 800, and idle byte-identically a minute later; a 50 ms cue interrupting an 800 ms one is over in 50 ms with BETA rather than ALPHA as its outgoing and no half-fade composite in it; the default record is still 500/800 to the millisecond, one millisecond short still mid-fade — an unconfigured service keeps its exact pixels); `scene_frame_tests::testVideoBackgroundNeverDissolves` (a 10 s background duration aimed at another video still lands the NEW background at once while both words are on that same frame; video→still bypasses in the other direction too); `worker_controller_tests::scenarioTransitionIntent` (d) (a capability-honouring peer's ledger: the go carries 100/200 fade, the clear's present says CUT, the restore carries the record captured WITH its retained content after the setting moved to 900/1000); `protocol_tests` and `rpc_ipc_tests`/`worker_rpc_lifecycle` (the name present in the hello and the count tripwires at 21); `render_scene_transition` and `scene_render_tests` (the default record blends exactly as this library always blended); `preview_budget_tests` (its 0.8 s ruler is now the CONTRACT's default rather than a renderer private, still honest because those presents carry no durations); `live_fade_tests` and `background_media_tests` (live fades and same-video continuity through a lyric change). Gates: desktop 65/65 serial, headless 36/36, `git diff --check` clean |
| TR.3 | DONE | Service and selected-view transition UI with end-to-end acceptance | Two doors, ONE record, and the audience as the witness. The read surface and the verb now spell the record identically: `DocumentViews::transitionRecordMap(optional<TransitionOptions>)` is the single author of the QML shape (`{kind, textDurationMs, mediaDurationMs}`, or an EMPTY map for a view that says nothing — there is no half-record default to invent) and `insertThemeTransitionRow()` puts `transitionSet` beside `transition` on every `themes()` row AND on `currentTheme()`, while `themeContext()` carries the pair for the named context; `DocumentController`'s private `transitionToMap` is deleted so a read can never offer a key its own write would refuse, and `setThemeTransition` / `setTextContextTransition` / `serviceDefaultTransition` / `transitionForSlide` all go through the one author. Movement travels with the entry it belongs to: `applyThemeContext` REPLACES a `TextContext`, so it now carries the existing entry's `transition` across when `fields` names no movement — absent keys are silence about the field they name and a caller editing the reading font named nothing about how the view arrives, which makes `transition` the ONE field silence does not take away (documented on the header, because the alternative is a typography edit quietly taking a view's Crossfade off). Settings' TRANSITION section and the Theme Editor's Transition section (Songs column = the Theme record; Bible fields = the `scripture` context's) each commit kind plus BOTH durations as one atomic record, because the file, the wire and the domain all refuse a fragment, so one gesture costs one undo and a half commit is not something a page can do by accident. Three choices and no fourth: Inherit WRITES NOTHING (the removal, enabled only against a record that exists — a button writing "nothing" over nothing costs an undo for no change), Cut and Crossfade each write a WHOLE record seeded from what was on show. A silent view shows what silence resolves to — the page reads the two records in front of it (`context → theme → service`, the order a Go walks) rather than re-walking a chain, so it cannot disagree with `transitionForSlide`, and the source line says who answers ("this theme answers for itself" / "inherits this theme — the theme carrying this view answers before the service" / "inherits the service") and says a Cut on its own terms instead of quoting times it ignores. Duration fields are disabled for a Cut and for Inherit WITHOUT being erased: the record still holds them, so coming back to Crossfade comes back to the times that were there. Validation is split where the knowledge lives: QML checks SHAPE only (digits, no sign, no point; anything else is -1, never a silently-rounded 0) and the 0..10000 band stays `Documents::validTransitionOptions`' one sentence, quoted by the settings refusal and by the worker's parse — `document_present_tests` proves the same `10001` earns the same words naming `textDurationMs` and `0..10000` over a real socket; a refused or non-numeric commit re-reads the model so a field snaps back to what the service holds, and a commit of an unchanged value writes nothing and costs no undo. The video-background limit is stated beside the background duration in both doors (and the service's own note says the same), and both say what the operator is really editing: the NEXT cue, because the record a cue moves with is captured when Go is pressed. Evidence that it is the audience's milliseconds and not the widget's: `frontend_journey_tests` leg 10 drives `DocumentController` verbs through `main.cpp`'s own resolver wiring to a real worker and times the operator's preview — (a) a service default of 2600 ms is seen as an early blend AND a landing no earlier than 1700 ms, (b) the view's Cut shows no frame holding both grounds and lands in a fraction of the same slide's crossfade, (c) changing the view mid-fade does NOT shorten the fade already crossing and the very next cue is a Cut, (d) editing a theme nobody wears leaves the acknowledged revision, the pending count and the picture alone, (e) an explicit `clearAll` reaches black without crossing whatever the view's numbers say while a `clearSlide` restore crosses its words in over the length the CLEAR captured — TR.2's rule read off the pixels, an edit after the clear changing nothing — (f) a cued go with duration keys still marks LIVE only when the worker names that revision active, (g) the saved service resolves to the record that drove the audience tonight, and (h) a restarted worker streams uniformly black with nothing on air while its hello still claims `scene.transition-options.v1` and the same slide still answers 2.6 s, so the contract is the build's rather than one process's. The two clocks this fixture reads are documented as not-one: `scenePending` sleeps on the present's REPLY and `sceneActive` moves on the next 500 ms status poll, which is why the off-air baseline waits for the acknowledged number to hold still longer than a poll instead of trusting the first quiet moment. `workspace_ui_tests` drives the controls themselves (real `clickAt` and combo activations, scrolled to where a long rail puts them): Settings' kind/numbers/disable-without-erase/undo; the Songs picker refusing Inherit until a record exists, Cut seeding from what was shown, typing one duration keeping its neighbour, an unchanged re-commit costing no undo, `10001` refused with the record unmoved, Inherit surrendering the record and one undo bringing picker and record back; the Bible door showing the THEME's 90/800 with "inherits this theme" (and not the service) for a silent view, writing its own record, naming itself as the source, keeping the view's Cut through a `contextSize` commit, and handing movement back without touching the reading size — the fixture leaving theme, context, view and window as it found them. `document_present_tests` closes the middle: the resolver's own answer for a UI-written theme goes on `scene.present` and LANDS, a key-less present is still the 500/800 a pre-TR.1 frontend performs, a Cut carries its durations whole, and an inherited theme resolves back to the service's pair which the worker accepts on the same keys — the door the UI opens and the door the wire has are one door. No format or version moved: TR.3 exposes records TR.1 stores and TR.2 draws. Gates: desktop 65/65 serial, headless 36/36, `git diff --check` clean |

| ID | Status | Integrated acceptance | Evidence |
|---|---|---|---|
| TA.1 | TODO | Close TV/TS/TR together; update architecture, captures, fixtures and handoff | New journeys prove selected view → styling/measurement → save/reopen → presentation; all leaf tests plus desktop serial/headless presets and source guards pass; report skips, font provenance, budgets and remaining physical gates separately |

Original suggested order (partly delivered; see CS for the current priority):
TV.1 → TV.2 → TS.1 → TV.3 → TS.2 → TS.3 → TV.4a →
TV.4b → TS.4 → TV.5 → TS.5 → TR.1 → TR.2 → TR.3 → TA.1, then M7.
Initial estimate **21.5 engineer-days**, 0.5–2 days per leaf including tests;
re-estimate after schema/renderer prototypes. Detailed dependencies and test
requirements live in the review; no new feature acceptance is claimed by TV.0.

### Implementation runbook — TV/TS/TR

This runbook is the implementation starting point; the linked review explains
the findings. Work on **one leaf at a time** in the order above. Read its steps,
the relevant source below and its tests before editing. New names below are
proposed APIs/fields, not a claim that they already exist. Preserve existing
callers with wrappers/default arguments while changing a seam. Do not start FO
or rewrite the renderer/document model to complete these leaves.

#### Source ownership and editing map

Paths below are relative to the repository root. Header `documents.h` means
`src/domain/include/openshow/domain/documents.h`; scene headers live under
`src/scene/include/openshow/scene/` and protocol headers under
`src/protocol/include/openshow/protocol/`.

| Responsibility | Starting files / existing functions |
|---|---|
| Domain types, mutations and resolution | `documents.h`, `src/domain/documents.cpp`: `Theme`, `TextContext`, `SlideCustomization`, `ResolvedAppearance`, `SlideItem`, `setThemeContexts`, `appearanceForSlide` |
| Service persistence and migrations | `src/domain/storage.cpp`: `kServiceFormat` (currently 9), theme/context/customization/item readers and writers; `src/domain/history.cpp` for value snapshots |
| Controller, QML adapters and undo | `src/ui/DocumentController.h/.cpp`: `edit`, `themeSampleSceneJson`, `themeContext`, `applyThemeContext`, `removeThemeContext`, `setThemePlacement`, `duplicateTheme`, `scriptureBoxStyle`, `scriptureSlideSceneJson`, `addScripturePassage`; `src/ui/DocumentViews.cpp` for exposed appearance rows |
| Workspace and theme gestures | `src/ui/Main.qml`, `ThemeEditorPage.qml`, `WorkspacePanels.h/.cpp`, `TopBar.qml`: page anchors, `syncInspector`, `syncContext`, `boxFor`, `endGesture`, `applyPlacementFields`, `sampleAsk`, `linesAsk` |
| Static compilation and scene validation | `src/scene/scene.cpp`, `scene.h`: `compileSlide`, equality, `toJson`, `fromJson`, scene format and strict per-kind keys |
| Font shaping, measuring and painting | `src/worker/render/SceneText.h/.cpp`: `SceneTextEngine::layout`, `layoutScene`, `drawLayout`, `GroupAlphaPainter`; `src/worker/session/OutputSession.cpp`: `measureText` |
| Scripture flow and transient state | `src/importer/include/openshow/importer/textflow.h`, `src/importer/textflow.cpp`, `scripturepagination.cpp`; `src/ui/ScriptureImport.h/.cpp`, `BibleImportPage.qml` |
| Async presentation intent | `src/ui/SceneAsk.h`: `ScenePrepareAsk`; `PresentationModel.h/.cpp`: `armPrepare`, `presentCueJson`, `handlePrepareAck`, `presentScene`, clears/restore; `WorkerController.h/.cpp` for public entry points |
| Worker command and frame state | `src/worker/rpc/RpcMethods.cpp`, `session/SceneStore.h/.cpp`, `session/OutputSession.h/.cpp`, `render/Layer.h`, `render/SceneRenderer.h/.cpp`: `present`, `Decision`, `presentScene`, `setActiveScene`, `captureFrame`, `commitFrame` |
| Protocol discovery and test registration | `src/protocol/protocol.cpp`, `protocol.h`, `preview_wire.h`, `src/ui/WorkerController.cpp` handshake; `tests/CMakeLists.txt` for new targets/resources |

#### Decisions to implement consistently

- **View identity:** UI label Songs maps to `textContext == ""`; Bible maps to
  `"scripture"`. Keep these persisted spellings. Theme selection and view selection
  are independent UI properties. Selecting an absent Bible view previews inherited
  defaults; it does not create a context or mark the document dirty.
- **Inheritance:** customization → first matching context record in current,
  slide, show, Default order → base theme chain → renderer defaults. Choose the
  context **once**, then resolve each field. Never search lower context records
  to fill holes. Unknown context names survive round-trip and theme duplication.
- **Format allocation:** current service/scene/protocol are 9 / 4 / 0.18
  (TS.1 claimed the service and scene bumps; TS.3 the protocol minor for
  `text.measure.v2`; TS.4 the next minor for `font.list.v1`, which adds no
  persisted meaning; TS.5 claimed none — pinned fixtures and raster evidence
  ship no new stored or wire meaning; TR.1 the service bump to 9 for the
  transition record and minor 18 for `scene.present`'s optional durations,
  whose CAPABILITY TR.2 then earned by drawing them — the hello lists 21 and
  no version of any kind moved, because TR.2 changed the pixels rather than
  the wire; TR.3 claimed none either, its two doors exposing records TR.1
  stores and TR.2 draws). Each leaf
  adding persisted
  meaning claims the next unused service format, adds migration/old-era-key
  refusal tests and updates version documentation. TS.2 advertises rendering
  support only when it actually draws the fields. Never bump versions blindly
  from the numbers printed here after another leaf has landed.
- **Style fields:** add `fontWeight` (integer 100–900, default 400), `fontStyle`
  (`normal`/`italic`, default `normal`), `fitMode` (`shrink`/`fixed`, default
  `shrink`), `minFontSize` (1–4096, default 12). Keep existing `fontSize == 0`
  meaning worker default (currently 96); do not use 0 to mean inheritance in a
  sparse override. Shrinking may never enlarge the selected font.
- **Effects:** `stroke = {enabled, color, width}` with RGB colour and width
  0–32; `shadow = {enabled, color, opacity, offsetX, offsetY, blurSigma}` with
  opacity 0–1, offsets −256–256 and sigma 0–64. Numbers are finite authored
  canvas units, scaled once at draw time. Defaults are disabled, stroke black
  width 2, shadow black/0.65/(2,2)/sigma 4. Width 0 or opacity 0 draws no ink.
  Use a complete validated record when an effect is present; absent optional
  record inherits, `enabled:false` explicitly disables the inherited effect.
- **Sparse editing:** new style fields are optional on Theme, customization and
  context records; resolve the documented concrete defaults only after the
  chain is exhausted. Do not populate old Theme records with explicit overrides
  during load. Update equality, empty/clear/has
  helpers, validators, JSON, row adapters and duplication together. No field
  may be editable if save/reopen or worker rendering drops it.
- **Bible roles:** first increment uses disjoint body/reference/attribution boxes,
  not shared-region caption placement. Existing context box/inset controls the
  body. Add optional `scriptureLayout` on the Bible context containing caption
  boxes and sparse caption text-style overrides; no second body rectangle.
  Do not persist role content in themes. Legacy untagged slides keep their pixels.
- **Transition fields:** `TransitionOptions {kind, textDurationMs, mediaDurationMs}`;
  `kind` is `fade` or `cut`, durations are integers 0–10000, defaults 500/800.
  Store one concrete `Documents` service default, optional whole-record overrides
  on Theme (Songs) and TextContext (Bible), and no per-slide override initially.
  Resolve the first matching context override, then first base theme override,
  then service default. A missing record inherits; an explicit Cut is an answer.
- **Runtime isolation:** QML never shapes text; frontend themes never cross RPC;
  no per-layer targets or `saveLayer`; no network/provider calls during preview
  regeneration or saved-passage playback. Fonts are discovered via the worker,
  not a second Qt-only font list. Future NDI/stage remain disabled.

#### TV.1 — selected-view fixture and edit routing

1. Add `selectedView` to `ThemeEditorPage.qml`, default `""`, with stable object
   names `themeViewSongs` / `themeViewBible` for navigation tests. Keep selected
   theme identity unchanged when switching views; cancel an unfinished drag.
2. Extend `themeSampleSceneJson(themeId, contextName)` and add a read-only
   `themeSampleAppearance(themeId, contextName)` adapter. Both must share one
   fixture builder and production resolution. Build it from a **copy of the real
   Documents**, create temporary show/slide, clear the copy's current theme, set
   the selected theme as that show's default, set the slide context, and compile.
   This preserves the real edited Default and all contexts without hand-copying
   selected fields. Never call `edit`/save/present on the real controller here.
3. Return the effective primary rectangle/typography from the compiled fixture;
   use that rectangle for the overlay and fields. Songs fields route to existing
   `editTheme`/`setThemePlacement`; Bible fields route to `applyThemeContext`.
   When writing one Bible field, merge the **current entire entry** and replace
   only that field. Do not run the old toggle's seed-from-base code implicitly.
4. Bible drag/typed box writes one context `box` and removes `inset` in the same
   edit; clearing the Bible box removes its `box`/`inset` keys only. Distinguish
   this from removing the whole view. Empty numeric fields clear the override;
   a present numeric zero remains an explicit value where the domain permits it.
5. Test sample JSON and effective geometry for Songs vs Bible, no Bible record,
   unknown context, modified Default and a current theme different from the
   selected theme. Assert real history/dirty/current-theme/output unchanged by
   browsing. Tests: `document_controller_tests`, `workspace_ui_tests`,
   `preview_selection_tests`. Do not change service format for this leaf.

#### TV.2 — reclaim workspace and prove reachability

1. In `Main.qml`, derive `themeMode` from `page === "themes"`. Hide library,
   output dock and media bin in this mode and anchor ThemeEditorPage to the root
   edges below the top bar. Anchor to the root explicitly; invisible docks still
   have geometry. Leave `WorkspacePanels` stored values untouched.
2. Replace theme-page fixed-height/plain columns with bounded layout containers:
   roughly 220 px left navigation, 300 px inspector, minimum positive centre.
   Put all inspector content in `ScrollView`/`Flickable` with correct content
   height and clipping. Give New/Duplicate/Delete their own fitting layout.
   Fit the canvas by `min(availableWidth, availableHeight * 16/9)` and centre it;
   overlay transforms must use the actual displayed canvas origin and scale.
3. Add Songs/Bible thumbnail rows via the existing bounded off-air thumbnail
   transport; use separate selection/thumbnail request generations. Avoid a new
   preview stream/renderer. Keep compact audience state and emergency clears in
   the header; mode exit cancels gestures and hands focus back to the workspace.
4. In `workspace_ui_tests`, use mouse/key events, scroll the inspector to its
   final control, and check the control's scene rectangle intersects the visible
   viewport before clicking. Merely finding a QML object or calling its handler
   is insufficient. Capture both views at all three ledger sizes and test panel
   widths/drawer state unchanged after entering/leaving Themes.

#### TS.1 — model, persistence and scene contract before controls

1. Define shared style enums/records and validation helpers in the Qt-free domain
   headers, using the exact defaults/limits above. Add fields to theme base,
   customizations and resolved appearance; TV.3 adds them to contexts. Expose
   undoable controller verbs with strict QVariant type/unknown-key checks rather
   than converting malformed strings to zero. Use `DocumentController::edit`.
2. Walk every consumer: domain equality and customization clear/has/empty;
   theme duplication; storage read/write/migration; resolver; scene compilation,
   equality/codec and budgets; controller theme/appearance rows. New fields must
   survive edit → duplicate → save → reopen → compile.
3. Read every currently supported service version. Absent new fields reproduce
   normal 400 / shrink / 12 / disabled effects; do not insert customization
   records into old slides. Refuse new fields under an older declared format.
   Normalize complete effect records so save/load does not change their meaning.
4. Add the next scene schema, retain a reader for legacy scene 3 with legacy
   defaults, validate new per-text-item keys and prohibit them on image items.
   Budget worst-case metadata within the RPC cap. Add capability spelling
   `scene.text-style.v1` to the contract, but advertise it only with TS.2's
   renderer; frontend must reject non-default unsupported styles before sending.
5. Extend `domain_tests`, `scene_tests`, `protocol_tests` and controller tests
   for min/max/out-of-range/NaN, disabled-vs-inherit, undo, malformed fields,
   format-era refusal, legacy equality and maximum-size scenes. No effects UI yet.

#### TV.3 — full sparse Bible view and inheritance provenance

1. Add optional `textColor`, `backgroundColor`, `backgroundGradient` and TS.1
   fields to `TextContext`; keep font/box/inset names unchanged. A present empty
   gradient explicitly means flat; absence inherits. Extend `setThemeContexts`
   validation, strict context JSON keys and controller map adapters together.
2. Preserve the resolver's first-context selection. Apply the chosen context's
   new fields above base themes and below slide customizations. Report source
   kind and theme/context identity per editable field without changing scene
   pixels. Use one resolution path for editor rows and compilation.
3. Allocate the next service format; format 7 remains typography/geometry-only.
   Add a migration fixture with several unknown context names and verify all
   survive duplicate/save/reopen. Test a high-priority partial context does not
   pull missing fields from a lower theme's namesake. Assert Songs unaffected.
4. Reuse the existing per-field revert idiom: clearing a view override removes
   that optional field, while clearing a slide customization resumes inheritance.
   Test effects disabled over enabled base, explicit flat gradient, edited Default,
   whole-view removal, one undo and unchanged other contexts.

#### TS.2 — render faces and effects from resolved styles

1. Pass weight/slant into public `TextStyle::setFontStyle`; include them in all
   shaping identities. Keep existing default family fallback and thread-owned
   FontCollection/paragraph rules. No system font installation in the product.
2. Use the same shaped glyph positions for shadow → stroke → fill. Prototype
   stroke using public paragraph foreground-paint / ParagraphPainter APIs in the
   pinned Skia; never shape a second differently styled paragraph to approximate
   an outline. Suppress duplicate shadows when painting stroke/fill separately.
3. Extend `GroupAlphaPainter` so the frame fade multiplies each effect's original
   alpha exactly once, including shadow opacity. Use public SkPaint stroke and
   blur APIs with authored-unit scaling; keep bounded preparation outside draw.
4. Add raster assertions for edge order, offsets, disabled identity, normal/bold/
   italic and 50% fades. Exercise the GPU live fade path too. Only then advertise
   `scene.text-style.v1`; unsupported-worker tests must still refuse non-default
   effects. Run `scene_render_tests`, `unicode_text_tests`, `scene_frame_tests`,
   `live_fade_tests`, `skia_public_api_guard`, `render_layer_guard`.

#### TS.3 — measurement, bounds and fit use the same style

1. Add ink bounds (left/top/right/bottom relative to text origin) to layout and
   measurement results while retaining paragraph height/line count. Derive glyph
   bounds from the same laid-out runs and expand for stroke and the finite Skia
   blur-filter support; shadow expansion unions offset shadow with fill bounds.
   Validate actual raster ink lies inside reported bounds, including negative
   offsets. Do not substitute a font-size-based height guess.
2. Add `text.measure.v2` capability and its style/bounds fields; keep v1 valid
   for old clients. Update `RpcMethods`, `OutputSession`, `WorkerController`,
   `textflow` MeasureRequest/Reply, scripture reservation and editor probes.
   An effects-dependent measurement on a v1-only worker must fail clearly.
3. Cache keys include lines, family, weight/slant, size, width, spacing, fit and
   effect records; cache generations include connection and resolved geometry.
   The current `(lines, fontSize)` importer cache is not enough across styles.
   A fresh run may clear a cache, but late replies must not refill a newer run.
4. `shrink` preserves the existing bounded loop/minimum default and uses visual
   bounds; `fixed` lays out once and reports overflow. Bible flow measures fixed
   size and repaginates words; it does not silently shrink to hide a bad plan.
   Tests compare measured vs rendered ink at 1920×1080 and scaled output, plus
   wide strokes, negative shadows, minimum-size failure and cache invalidation.

#### TV.4a — persist semantic scripture items and layout slots

1. Add an optional text role on `SlideItem`: absent/legacy, `scripture-body`,
   `scripture-reference`, `scripture-attribution`. Refuse roles on image items
   and unknown role spellings; version the persisted keys. Legacy load leaves
   roles absent. Do not infer them from first/second/third stack position.
2. Add `scriptureLayout` to the Bible context with reference and attribution
   slot records (box + sparse text-style fields). Body style/geometry comes
   from the existing resolved Bible view. A caption inherits family/fill/effects,
   with its explicit size/alignment overriding the body. Keep geometry finite,
   positive, inside 1920×1080 and caption/body regions disjoint for generated
   slides; invalid layouts disable generation with an actionable error.
3. Provide an explicit **Add Bible caption layout** action; selecting Bible or
   loading an old service must not add it. Initial recipe: body `(160,100,1600,680)`,
   reference `(160,800,1600,64)` at 28 px left aligned, attribution
   `(160,884,1600,96)` at 22 px left aligned. Existing box changes happen only
   when that action is chosen, in the same undo edit. Reject/surface captions
   that do not fit; operators may adjust the boxes and sizes.
4. Add a role-aware resolver used by `compileSlide` to produce ordinary resolved
   `SceneItem`s; roles do not become worker template bindings. Placement and
   placement customization target the tagged body; use legacy first-text
   placement only when no tagged body exists. If the current theme has no caption
   layout, tagged captions retain authored geometry and role defaults (28/22 px,
   left aligned), inheriting other text fields. Test deleting/switching themes,
   reordered captions, extra text/images, migration identity and item-count cap.
5. Add a Body/Reference/Attribution selector in the Bible inspector. Body edits
   existing view fields; captions edit the selected `scriptureLayout` slot via
   one whole-context merge. Revert removes only that slot's optional text field;
   deleting the layout is a separate explicit action and never deletes slide words.

#### TV.4b — generate the same roles that were measured

1. Extend the generated-slide payload to carry body lines, verse-range label,
   reference/translation text, attribution and their semantic roles. Preserve
   verse label in notes and original provenance on the schedule-owned show.
   Use one shared fixture/compiler path for importer preview and final Add.
2. When a caption layout is enabled, paginate against the actual body box;
   measure reference/attribution at their own style/width in their separate
   boxes. Do not subtract the old caption reservation from that body a second
   time. Without a new layout, retain the existing legacy pagination path.
3. Extend `addScripturePassage` to validate all role items and target first,
   then write one schedule-owned show/playlist entry in one `edit`. On any
   refusal leave no partial show/items/assets; never promote it into Library.
4. Cover long/multiline attribution, empty optional caption, impossible role
   boxes, word conservation/continuations, preview→Add equivalence, one undo,
   save/reopen and offline playback with network request count unchanged.
   A later theme change restyles these authored items without changing boundaries.

#### TS.4 — expose controls through the worker's fonts and view verbs

1. Add bounded `font.list` RPC/capability `font.list.v1` through the worker font
   manager, returning family/face/weight/style metadata, not font bytes. Cache
   per worker lifetime; cap reply bytes (page it if needed), and refuse a stale
   connection response. Keep scanning out of presentation/audio callbacks.
2. Add a searchable family/face popup and font sample; show requested vs missing
   family/fallback status without rewriting the stored requested name. Support
   typed names even when the current worker is disconnected.
3. Put font weight/style, fill, stroke, shadow and fit controls into the TV.2
   inspector. Route to base theme for Songs or sparse context for Bible; show
   inherited-source badges. Use validated one-field merge verbs rather than
   extending a fragile positional `editTheme` call with many arguments.
4. On refusal re-read authoritative values, on focus-out without change write
   nothing, and batch drag/spin gestures into one undo. Test actual popup search,
   scrolling to shadow blur, inherit/off/revert, disconnect/reconnect and saved
   scene equality with the preview; no QML-only text effects.

#### TV.5 — state generations own previews and Add

1. Track a monotonically increasing editor generation keyed by selected theme,
   view, resolved style and worker connection. Invalidate pending line probes and
   selection readiness immediately on change; old reply IDs cannot update labels
   or show an old image beneath current handles. Include font family in probes.
2. In `ScriptureImport`, retain an input fingerprint/generation covering ordered
   selected verses, translation/attribution, complete resolved role styles/boxes,
   worker connection and **target schedule identity**. Capture that target with
   the plan; Add must not read a newly selected playlist and reuse old slides.
3. Re-entering Bible regenerates if the fingerprint differs, even if theme
   changes happened while the page was hidden. Before Add, compare again; on a
   mismatch invalidate and regenerate, leaving Add disabled. A timed-out/dead/
   superseded measurement has no route to mark a newer plan ready.
4. Do not regenerate synchronously from the Add mutation's `documentsChanged`
   signal and accidentally enable a duplicate insertion: mark the batch consuming
   before commit and clear it on success. Test hidden-page edits, delayed A→B→A
   replies, target switch, disconnect/reconnect, double Add and cancelled drag.

#### TS.5 — pin fonts and record visual/resource evidence

1. Use the review's pinned Google Fonts revision, TTF hashes and each `OFL.txt`.
   Create an explicit setup script/manifest with hash checks, never a network
   download inside ctest. Use a local cache and isolated Fontconfig/cache path;
   vendor fixtures only if the packaging/licence policy allows it. Include a
   pinned fallback face and ensure family resolution is asserted before pixels.
2. Parameterize real text-engine fixtures over Inter/Lora/Oswald, fallback and
   Unicode. Test body/caption metrics, extreme legal effects, fixed/shrink and
   fading; use raster tolerances rather than driver-dependent PNG byte equality.
3. Record steady-state cache/build counts and frame/preparation budgets with
   previews active; assert finite cache occupancy after churn and no new shaping/
   decode work on a steady draw. Report setup skips separately from passing tests.

#### TR.1 — capture transition options at Go, not at prepare ack

1. Add shared Qt-free `TransitionOptions` with one validator. Add undoable
   service-default and optional Theme/TextContext setters plus storage migration,
   duplication and `resolveTransitionForSlide(showId, slideId)` following the
   transition-record priority above. Do not put durations in static SceneItem.
2. Resolve options in the cue/controller entry point when Go is issued. Carry a
   **value copy** on `ScenePrepareAsk` and the corresponding pending Go record.
   `handlePrepareAck` forwards that intent's copy; it never consults mutable
   current settings. Legacy present entry points explicitly supply defaults.
3. Extend `scene.present` with optional `textDurationMs` / `mediaDurationMs`
   alongside existing `transition` (`fade`/`cut`). Missing fields use 500/800;
   malformed/fractional/negative/>10000 values refuse before touching SceneStore,
   including when kind is Cut. Advertise `scene.transition-options.v1` only with
   TR.2; an old worker accepts only legacy-equivalent settings via the frontend.
4. Carry the complete record through SceneStore desired state and `Decision`,
   OutputSession's landed scene handoff, and the renderer. Keep acknowledged
   live markers and repeat-present no-op rules. Add delayed-ack test: Go A with
   100/200, change settings to 900/1000, receive A's ack; A must still send 100/200.

#### TR.2 — landed transition owns durations and completion

1. Replace renderer constants with options captured in `SceneTransition` on
   `setActiveScene`; defaults remain 0.5/0.8 seconds after one ms conversion.
   Blend helper checks `duration <= 0` **before** `elapsed <= 0` so zero duration
   reaches 1 on the first frame. Cut/first scene discard outgoing fade state.
2. Capture blends once per audience frame and retire the outgoing scene only
   after the settled frame is actually presented. A failed submit/present still
   owes that frame; preview drawing cannot move the clock or retire the fade.
3. Explicit clears send Cut. Store restore options with the retained restore
   point and snapshot them into the Restore intent; do not use whatever theme
   the operator happens to be browsing. Preserve current rapid-Go replacement
   semantics without compositing the in-progress blend to a new render target.
4. Test 0/0, 0/800, 500/0, 100/200, Cut interrupt, Go during fade, failed final
   present, clear/restore and idle after completion. Keep same-video background
   playback continuous; different-video background switching remains a cut.
   Run CPU frame/store tests, developer transition check and real live fades.

#### TR.3 — service defaults and view overrides in the UI

1. Put service defaults in Settings and a Transition section in Theme Editor.
   Offer Inherit, Cut or Crossfade for view overrides; show inherited source and
   resolved foreground/background milliseconds. Disable duration fields for Cut
   without erasing them. Songs writes the Theme record; Bible writes its context.
2. Entering fields validates through controller/domain and commits one undo.
   Show the existing video-background limitation beside the background duration
   control. Editing the selected off-air theme never presents a cue.
3. Extend `workspace_ui_tests` with actual control interaction, then a production
   journey that changes timing through these verbs and measures landed frames.
   Cover persistence, default inheritance, active-fade stability, clear/restore
   and worker restart to black. Do not infer success from the widget value alone.

#### TA.1 — completion and verification commands

After each leaf, build only what changed and run its listed meaningful tests;
after the complete milestone run both presets. Run display/IPC fixtures in a
session that permits local sockets, shared memory and the worker display. If a
restricted sandbox blocks those resources, record that environment failure and
rerun with access; do not weaken/skip assertions to obtain green output.

```sh
cmake --build --preset desktop --parallel 2

# TV/model leaves (add further exact test names when a leaf creates them).
ctest --test-dir build -R '^(domain_tests|scene_tests|document_controller_tests|workspace_ui_tests|workspace_panels_tests|preview_selection_tests)$' --output-on-failure -j1

# TS/render and measurement leaves.
ctest --test-dir build -R '^(scene_render_tests|unicode_text_tests|scene_frame_tests|text_measure_tests|live_fade_tests|skia_public_api_guard|render_layer_guard)$' --output-on-failure -j1

# TV scripture generation/state leaves.
ctest --test-dir build -R '^(textflow_tests|scripture_pagination_tests|bible_import_page_tests|bible_import_e2e_tests)$' --output-on-failure -j1

# TR async commands and real frame timing.
ctest --test-dir build -R '^(protocol_tests|worker_controller_tests|scene_store_tests|scene_frame_tests|render_scene_transition|live_fade_tests|document_present_tests|production_journey_tests)$' --output-on-failure -j1

# Repeat capture runs at different scale factors for the final UI gate.
QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software OPENSHOW_WORKSPACE_CAPTURE=/tmp/openshow-theme-acceptance.png build/tests/workspace_ui_tests

# Final milestone gates; headless preset excludes display-labelled tests.
ctest --preset desktop -j1
cmake --build --preset headless --parallel 2
ctest --preset headless -j1
git diff --check
```

Extend the capture harness to emit both theme views at each required size;
its current single `.themes.png` is not sufficient. Repeat capture invocations
with `QT_SCALE_FACTOR=1.5` and `2` and distinct output prefixes. Register any
new test target in `tests/CMakeLists.txt`, including display labels/timeouts
and deterministic font setup. A leaf is DONE only when its new behaviour is
covered and its named tests pass; fields merely serialized or handlers called
directly do not establish visual/interaction acceptance. Keep unfinished controls
disabled. Update architecture/handoff with actual commands/results, skips and
remaining physical gates, and preserve old DONE evidence. This user's current
GitHub publishing authorization covers the documentation update; follow the
session's authorization for later implementation commits.

### Shared Qt control sets and preview corrections (CS) — before FO

Review date: **2026-11-04**. This work addresses the operator's Bible Import,
Theme Editor and Edit screenshots. This is a source review and implementation
brief; no runtime fix or visual acceptance is claimed by this documentation.
Keep the existing DONE ledger intact. FO remains deferred.

#### Findings and boundaries

- `PreviewView.h/.cpp` already supplies the common worker-pixel item with
  **Audience**, **Selection** and keyed **Thumbnail** targets. It aspect-fits
  the frame; Qt does not render the slide's words. Reuse it and extract the
  surrounding QML surface/card controls rather than introducing a renderer.
- `BibleImportPage.qml` uses verse checkboxes followed by a `Repeater` of
  `Frame`/`Text` generated-slide rows. `ScriptureImport::publish()` calls
  `previewSlide(0)`, which sends a Selection request, but the Bible page has no
  Selection surface. `OutputPanel.qml` displays **Audience**, so the existing
  sentence promising an off-air picture in the right preview panel is wrong.
  Add a clearly labelled off-air canvas in the Bible page itself. Keep the
  right Audience monitor showing the live output.
- `ThemeEditorPage.qml` and `EditPage.qml` independently implement colour,
  family, size, alignment, spacing, placement and field synchronization. The
  former also has TS typography/effects, a worker font browser and transitions;
  the latter's appearance panel only exposes the basic fields. Domain records
  and controller verbs already share many of these meanings; the visible
  controls and their write routing are the missing shared layer.
- `EditCanvas.qml` and the Theme Editor each put handles over worker pixels,
  but edit different geometry: the selected authored item versus the theme's
  primary placement/context box. Share the surface and geometry fields first;
  keep these gesture owners separate until their differences are explicit.
- `ShowPage.qml::cueRows` already carries `entryId`, `cueKey`, `slideId` and
  `groupStart`, but one `GridView` packs all cues continuously. Its `slideBand()`
  assumes visual cell number equals cue index. Adding row breaks must change
  both layout and viewport scheduling, while preserving cue identities.
- Existing useful containers include `OutputPanel`, `PlaylistPanel`, `SidePanel`,
  `PanelField` and `PanelButton`. Compose with these; keep audience clears,
  transport and audio in `OutputPanel`. This scope does not require splitting
  every QML file or moving presentation logic into generic widgets.

#### Named parts — vocabulary and intended ownership

These names are **proposed**, except `PreviewView` and `OutputPanel`, which
already exist. Use the names verbatim in future requests (for example,
“BibleImportWorkspace's SlidePreviewSurface” or “Edit's TextStyleControls”).
Prefer QML composition with explicit properties/signals over a page inheritance
hierarchy. A theme view and a slide customization inherit appearance values
through the domain resolver; QML inheritance must not reimplement that chain.

| Part name / proposed file in `src/ui/` | Responsibility and intended consumers |
|---|---|
| `SlidePreviewSurface.qml` | Aspect-fit worker pixels, frame/status/placeholder and overlay slot; Bible, Edit, Themes, Show card and Audience monitor wrappers; wraps existing `PreviewView` |
| `SlidePreviewCard.qml` | Surface plus caption, selected/pending/live chrome and activation/context signals; Show cues, Bible generated slides and Themes view tiles |
| `SlidePreviewCollection.qml` | Virtualized responsive preview collection, group boundaries, index mapping and visible-band reporting; Show first, reusable ungrouped mode for Bible |
| `AppearanceInspector.qml` | Scrollable composition of supported style sections; Edit and Themes supply different target adapters |
| `TextStyleControls.qml` | Family, size, alignment, line spacing, weight, slant, fit and minimum size; same controls for supported theme/view/slide targets |
| `TextEffectsControls.qml` | Outline/shadow whole-record editing, enable/off/inherit state and resolved values; Themes and Edit |
| `BackgroundControls.qml` | Colours/gradient plus supported image/fit controls; expose only operations the current target can store and render |
| `BoxGeometryControls.qml` | Shared X/Y/W/H inputs and follow/inherit action; Edit item/customization and Themes placement/context adapters |
| `TransitionControls.qml` | Cut/Crossfade, durations and optional Inherit/source text; Themes and Settings; no new slide transition override |
| `WorkerFontPicker.qml` | Search/browse worker-owned family/face metadata, missing-font state and typed-name path; TextStyleControls |
| `BibleImportWorkspace` (`BibleImportPage.qml`) | Lookup, verse selection, generated-slide collection, selected off-air canvas and Add/Cancel; owns import interactions |
| `AudienceMonitor` (inside existing `OutputPanel.qml`) | Live picture/status and existing clears; instance of the common surface, with output-specific operations outside it |

CS.1 must record each part's inputs, events, adapter and consumers in a new
`docs/ui/control-sets.md` (a component catalogue, not a second task ledger).
When implementing a part, add a short source header such as
`// UI part: SlidePreviewSurface; contract: docs/ui/control-sets.md#slidepreviewsurface`.
Use stable, unique per-instance `objectName`s, meaningful `Accessible.name`s and
stable field names for tests. Preserve existing automation names where possible;
document and update legitimate changes together. Names are source/UI vocabulary,
not service-format keys. Do not add persisted component identifiers.

#### Work ledger and order

Each implementation leaf targets 0.5–2 engineer-days including its focused
checks; split before starting if its prototype exceeds that. The estimate is
provisional, roughly **11–16 engineer-days for CS.1–CS.9**, excluding the open
TV/TA and M7 work. CS.0 records this review only.

| ID | Status | Work | Acceptance |
|---|---|---|---|
| CS.0 | DONE | Source review, named-part proposal and pre-FO dependency gate | Findings, source map and executable handoff below; documentation only |
| CS.1 | DONE | Establish part contracts, catalogue and QML resource registration convention | Explicit pixel targets, adapter/event contracts, stable names and module/resource rules documented for subsequent extractions |
| CS.2 | DONE | Extract common preview surface and slide card | Edit, Themes, Show and Audience wrappers share pixel/chrome controls; aspect, overlays, state and target isolation preserved |
| CS.3 | DONE | Replace Bible generated-slide text presentation with themed worker previews | Generated cards and selected off-air canvas use the current compiled scripture appearance; selection never presents; no overlapping controls or misleading preview instructions |
| CS.4 | DONE | Normalize inspector read state and mutation adapters | Shared field shape with resolved/own/source/capability state; theme, context and slide writes remain distinct, atomic and undoable |
| CS.5 | DONE | Extract style/geometry/font/transition sections and migrate Themes/Settings | Common controls replace duplicated sections; Songs/Bible differences and existing sparse-context limits explicit; all supported fields reachable |
| CS.6 | DONE | Migrate Edit to the shared inspector and expose supported advanced styling | Same typography/effects/font controls as Themes; writes customize only the current slide, reverts restore inheritance; authored item tools remain intact |
| CS.7 | TODO | Group Show previews by running-order entry with a fresh row per item | Every item starts at column zero, repeated entries stay separate, cue count/order/keyboard/live semantics unchanged |
| CS.8 | TODO | Make preview requests and scroll-to-cue use grouped layout geometry | Actual visible cues plus bounded look-ahead requested, gaps omitted, 32-scene cap/dedupe/coalescing preserved; bottom and zoom cases verified |
| CS.9 | TODO | Integrated reuse, accessibility, screenshot and regression acceptance | New controls exercised by real gestures and worker pixels; cross-page state isolation, save/reopen and current TV/TA completion recorded before FO |

Current priority: **CS.7 → CS.8 → CS.9**.
Then complete open **TV.3 → TV.4a → TV.4b → TV.5 → TA.1 → CS.9**, then M7.
TS.1–TS.5/TR.1–TR.3 and TV.1–TV.2 remain DONE. Their original suggested order
above is historical, not an instruction to redo them. Shared controls can land
against today's sparse Bible context; TV.3 later adds independent view style
fields to its adapter. TV.4 owns caption roles, and TV.5 owns full import-plan
validity across mode/theme/worker/target changes; reuse those leaves instead of
inventing competing schemas or duplicating their acceptance.

#### CS.1–CS.2 — shared preview contracts and extraction

1. Inspect `PreviewView`, `EditCanvas`, Themes' `previewFrame`/`ViewTile`, Show's
   tile delegate and OutputPanel's `previewBox`. Define required controller,
   explicit target/key, aspect ratio, loading/unavailable/current-frame state,
   caption/chrome and overlay slot. The surface displays only; page/session
   owners schedule scene requests and decide what activation does. A Show card
   may present; a Bible or Themes card only selects off air. Never put a default
   Go/clear operation in a reusable preview part.
2. Extract the smallest common surface/card and migrate existing consumers
   before adding Bible use. Keep `PreviewView` as the only pixel painter and
   keep worker text layout, shared-memory ownership and image caches unchanged.
   Bind Audience/Selection/Thumbnail explicitly. Missing thumbnail or selection
   pixels show an honest placeholder, never the Audience frame or a Qt text
   recreation advertised as the rendered slide.
3. Keep overlay coordinates in the same fitted content rectangle as the pixels,
   including letterboxing/resizing. Edit retains selection/stack hit testing;
   Themes retains theme-placement gesture routing. Controls/handles must not
   obscure the actual content or intercept unrelated card actions.
4. Register new QML files in `src/ui/CMakeLists.txt` through
   `OPENSHOW_WORKSPACE_QML_NAMES`; preserve resource aliases used by
   `tests/CMakeLists.txt` (it currently flattens filenames). Prefer flat files
   for this extraction unless aliases in both modules are deliberately updated.
   Build the application AND workspace fixture. Extend `workspace_ui_tests`
   and `preview_selection_tests` for fitting, target isolation, overlay mapping,
   captions, missing-worker state and unchanged output clears.

#### CS.3 — BibleImportWorkspace

1. Use a responsive layout: lookup/status first, a compact scrollable verse
   selection area, a generated `SlidePreviewCollection` (initially ungrouped),
   a substantial selected `SlidePreviewSurface`, then reachable Add/Cancel.
   Checkboxes are source selection controls; keep their readable verse text.
   Replace the generated-slide plain-text frames with rendered cards; optional
   text/details remain secondary and must not pretend to show final layout.
2. `ScriptureImport` owns selected generated-slide index and stable transient
   preview identity. Compile from `scriptureSlideSceneJson` and the same
   scripture fixture/resolver used by `scriptureBoxStyle`/Add. Use the active
   theme's Bible (`scripture`) context with its documented inheritance. Browsing
   an unrelated theme in Themes must not substitute that theme for the import.
   No network refetch or temporary document/history edit is needed for previews.
3. Introduce the collection's ungrouped virtualized mode here. Publish a bounded
   scene/key list for the current generated plan so cards can
   request worker thumbnails. Keys must distinguish plan generation and slide,
   not merely index `0`, `1`, etc. Use the existing thumbnail limits/cache and
   selected Selection target. Reuse the collection's visible-band mechanism;
   the grouped Show migration is CS.7–CS.8. Retire/disable hidden-page requests
   so Show, Themes and Bible cannot overwrite each other's active batches.
4. Clear or mask obsolete selected pixels on selection/plan/page/connection
   changes until the matching request is ready. Audit the current unkeyed
   Selection image's readiness/correlation before relying on it; extend the
   frontend request/readiness seam if needed. Cancel/Add completion must not
   leave old plan cards labelled current. TV.5 still supplies the full
   generation-bound Add/measurement validity contract before CS.9 closes.
5. Correct the preview-help/status wording. Do not add Present to this page or
   change the live Audience monitor. Preserve one Add = one undoable schedule
   item, double-Add prevention, notes/attribution and passage word conservation.
   Caption rendering itself remains TV.4; show exactly the scenes supported now.
   Verify through `workspace_ui_tests`, `bible_import_page_tests`,
   `bible_import_e2e_tests` and `preview_selection_tests`: multiple slides,
   current scripture font/box/colours/effects, selection, no worker, delayed
   replies, page changes, Add/save/reopen and zero audience presents from browsing.

#### CS.4–CS.6 — shared appearance controls with separate write adapters

1. Define a read model per field, for example
   `{value, ownValue, isSet, source, editable, canReset}` plus explicit target
   identity and supported sections. This is a proposed UI adapter shape; map
   existing `slideAppearance`, theme rows, `themeContext` and
   `themeSampleAppearance` into it in one place. Domain resolution remains the
   authority. A controls file must not walk theme chains or synthesize new
   inherited overrides merely to populate a text field.
2. Define `commit(field, value)` and `reset(field)` events. The host adapter
   translates them to `editTheme`/`setThemeTextStyleField`, context mutations,
   or `customizeCurrentSlide`/`revertCurrentSlideField` as appropriate. Settings
   supplies the concrete service transition target. Preserve the active theme,
   selected theme, view, slide and selected item as distinct identities. Capture
   the target at gesture start; cancel a pending gesture if that target changes.
3. Theme context writes currently replace a context via `applyThemeContext`.
   Preserve untouched members (including transition) and unknown contexts;
   centralize merge/preservation in the adapter/controller, not copied QML
   handlers. Avoid carrying stale snapshots across navigation. If controller
   helpers are required, add focused tests for preservation and refusals.
4. Keep “inherit/reset” separate from explicit flat gradient, platform family,
   renderer-default size, Cut and disabled effects. Effects commit complete
   records; a member edit merges once through a shared helper, and a reset
   removes the whole override where the existing contract requires it. Retain
   current domain validation bands and report refusals, resynchronizing from
   the authoritative record. Unchanged commits and rejected edits cost no undo;
   slider/geometry gestures commit once on release and undo once.
5. Move family browsing into `WorkerFontPicker`, fed by worker metadata. Typed
   names remain usable offline; missing-family warnings require a completed
   worker list. Do not use Qt's installed fonts as the audience font authority.
   Keep scalar/style controls, effect controls, background controls, geometry
   fields and transitions as composable sections with capability flags. Where
   today's Bible context cannot store an advanced field, state its inherited
   source and disable that edit until TV.3; never silently write the Songs base.
6. Migrate Themes and Settings first (CS.5), then Edit (CS.6). Edit gains the
   existing supported weight/slant/fit/minimum/stroke/shadow controls through
   customization verbs, with the same font picker and reset conventions. Extend
   its customized-field badges and “Remove customizations” enablement to cover
   advanced-only changes. Keep theme assignment and item-stack commands in host
   wrappers; selected extra-item geometry must not become theme placement.
7. Test both hosts with the same gesture scenarios: commit, no-op, invalid
   input, reset, undo/redo, switch target mid-edit and save/reopen. Assert theme
   edits affect intended users, slide edits leave the theme and other slides
   unchanged, and the compiled preview matches the stored effective values.
   Entry points: `document_controller_tests`, `workspace_ui_tests`,
   `document_present_tests`, `preview_selection_tests`; extend `domain_tests`
   only when domain behavior changes. No format/protocol bump for extraction;
   TV.3/TV.4 retain ownership of new persisted view/role meaning.

#### CS.7–CS.8 — show-item row boundaries and visible preview requests

1. Group by **running-order entry occurrence**, using `entryId`, not show name,
   theme, Bible context or `showId`. Two occurrences of one show are two groups.
   Library browsing is one group. Each nonempty group begins at column zero;
   slides within it flow left to right and wrap normally. The next group begins
   on the next row, even when the previous row has spare columns. No forced
   blank row if the previous group ends on a full row. A small heading/gap may
   identify the item; it is not a cue or a keyboard stop.
2. Implement `SlidePreviewCollection` using virtualized visual rows (group
   metadata plus up to N real cards per row), or an equivalently bounded model.
   Avoid an eager Column/Repeater of every slide or unlimited nested grids.
   Preserve flat `cueRows` as presentation order; maintain explicit mappings
   between cue index/key and visual row/cell. Empty shows create no phantom cue.
   Padding cells, if used, cannot receive focus, clicks, context menus or Go.
3. Replace `GridView.Contain`/index arithmetic with the mapped cue rectangle.
   Previous/next/PageUp/PageDown/Space cross group boundaries in flat cue order.
   Preserve cue keys across resize/zoom and keep live/pending markers based on
   worker acknowledgements. Right-click actions still address the clicked slide;
   repeated occurrences retain separate live identity despite sharing pixels.
4. Derive the thumbnail band from actual visible rectangles and at most one
   visual row of look-ahead, excluding headings/gaps/padding. Dedupe by slide
   identity, retain existing coalescing and the protocol's 32-scene/extent/cache
   bounds. If visible demand exceeds a batch, ensure all visible pictures are
   eventually served without a request loop; never let overscan displace visible
   demand permanently. Exercise top/middle/bottom, partial rows, resize, all five
   thumb sizes and scroll-to-last-cue. No unbounded request per group.
5. Extend `workspace_ui_tests` with deterministic services: a three-slide song
   followed by a three-slide Bible item with four columns (Bible starts on a
   new row), exact-full and multirow groups, repeated show entries, empty items
   and a single library show. Assert actual card positions, logical cue order,
   pointer/keyboard behavior and bounded visible-band contents. Update old tests
   that assume `cellIndex == cueIndex`; retain the behavior they protect.
   Run `frontend_journey_tests` and `production_journey_tests` for presenting
   across boundaries and ack-backed live markers with a real worker.

#### CS.9 — completion and handoff to other models

Read PLAN.md/PROGRESS.md, the component catalogue once CS.1 creates it, the
named source and tests, and Git status before each leaf. Deliver one leaf at a
time with changed files, exact checks/results, remaining limitations and next
task. Update this ledger and the short handoff; do not mark a part DONE from
file extraction or a control value alone. Keep unfinished behavior disabled.

Use the existing TA.1 build/test commands, adding the leaf-specific entry
points above. A full desktop build is needed to embed changed QML resources
in the application and fixture. At closure run desktop/headless serial gates,
`git diff --check`, and captures of **Show, Bible Import, Edit, Themes Songs and
Themes Bible** at 1000×680, 1380×800 and 1680×1020, including scale factors 1.5/2.
At narrow sizes allow a deliberate stacked layout/scrolling, with a useful
preview size and all form actions reachable. Check Tab/Enter/Escape, field focus
on page exit, accessible names, first/last inspector controls and no overlap.

Use real worker-frame evidence for theme fidelity and Audience/Selection/
Thumbnail isolation; mouse/keyboard tests must operate on visible controls.
Verify edits in one host refresh the other, no duplicate hidden-page requests,
no document/history change from browsing, and no audience change without Go.
Finish remaining TV/TA acceptance and update `docs/architecture.md`, captures,
the catalogue and PROGRESS.md with actual results. Physical M7 output checks
remain separate. FO is not the next task merely because these new controls exist.

### Future output profiles — lower thirds to OBS and stage (FO)

**Entry gate: CS.1–CS.9, the remaining TV leaves and TA.1 must be DONE, with
the current one-audience release gates satisfied, before starting FO.1.**
Section order is not authorization to bypass those dependencies.

Deferred beyond TV/TS/TR/CS and the current one-audience release. Content view
(Songs/Bible) and destination (audience/broadcast/stage) are separate dimensions.
Do not implement NDI as a slide context or advertise an active sender from an
editor placeholder. Spike first and split any leaf exceeding two days.

| ID | Status | Work | Acceptance |
|---|---|---|---|
| FO.1 | TODO | Define independent output profiles, per-target scene/state/preview ownership and reflow policy | Same cue resolves different Songs/Bible layouts for three targets without changing audience pagination or leaking stage-only content; migration and isolation fixtures |
| FO.2 | TODO | Alpha-aware broadcast/lower-third compositor and preview | Transparent clears/background omission, premultiplied edges/effects and sizing tested over checkerboards; audience remains opaque and unchanged |
| FO.3 | TODO | NDI sender dependency/build and OBS compatibility spike | Record SDK/distribution requirements, supported alpha/pixel formats, receiver/plugin setup, frame pacing and measured network/resource budgets on target machines |
| FO.4 | TODO | Bounded paced NDI sender and destination controls after spike | Real OBS receives lower thirds/alpha; discovery/reconnect/disable work; sender pressure/loss never stalls audience, previews or audio |
| FO.5 | TODO | Stage layout, current/next/notes bindings and independent output | Off-air and on-device tests prove correct current/next content, transition policy and no audience leakage; independent destination/removal lifecycle |
| FO.6 | TODO | Physical multi-output and network acceptance | Mixed service with projector, OBS receiver and stage, disconnect/reconnect/backpressure and resource budgets recorded; packaging/notices and recovery gates extended before enabling release |

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
