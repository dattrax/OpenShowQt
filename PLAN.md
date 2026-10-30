# OpenShowQt: implementation plan

Accepted 2026-10-21; condensed and audited 2026-10-29.
Current checkpoint and verification: [PROGRESS.md](PROGRESS.md).
Current code boundaries and open differences: [docs/architecture.md](docs/architecture.md).
Detailed completed-task evidence: [history](docs/history/README.md).

## Goal and reference

Build a Linux-first C++/Qt presentation application closely reproducing FreeShow's
core operator screens: service playlists, lyrics, visual editing, images, video,
audio, operator previews and one audience display.

Reference: `/home/jim/FreeShow`, FreeShow 1.6.7 at
`436d4b634878ba723da1d1373637384ec700ce16`. Leave that clone unchanged.
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
  callbacks. Prepare-time file work left the RPC loop for the bounded prepare
  executor (6.9a), and a background video's bring-up is primed there too — the
  render loop only attaches its GPU half at the frame boundary (**6.9b**).
- **RPC:** length-prefixed JSON-RPC 2.0 on Unix-domain sockets; Qt local sockets
  in the frontend and a raw socket/poll loop in the worker. Version/capability
  negotiation, request IDs, structured errors and acknowledged revisions. Traffic
  carries bounded scene/control metadata and paths, never original/decoded media.
- **Preview:** composed worker pixels through three POSIX shared-memory slots;
  exact stream/sequence ready-release ownership. Normally 640×360 at up to 15 fps
  on content changes; selected editor scenes and on-demand thumbnails are bounded
  separately. Drop/defer previews under load; never overwrite a consumer-owned slot.
- **Audio:** native PipeWire, F32 stereo at negotiated rate, independent gain/mute
  for video and one standalone track. The original design uses one software-mixed
  stream; production currently uses two independent transports/streams. **6.20 must
  reconcile this explicitly.** Audio is the clock when running, monotonic otherwise.
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
- **Deferred:** FreeShow import/round-trip, stage/scripture/additional outputs,
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

Production foreground images and text currently share one authored item stack
under the words layer; the images layer paints only the development decoration.
Independent image/word controls versus mixed z ordering must be resolved in
**6.20**, not hidden by a documentation rewrite.

### Responsibility boundaries

OutputSession coordinates window/device/swapchain, frame capture and presentation.
SceneRenderer owns layers/transitions; SceneStore owns command/scene truth.
Preview production, background transport, frontend RPC/consumer/supervisor and
save/UI components have explicit pending extraction tasks. Development fixtures
must drive those production seams; commands must not manipulate hard-coded demo
state. Keep a single document/history owner and a single worker text renderer.

Skia is pinned in `third_party/skia`; the build recipe and configuration live in
`scripts/build-skia.sh` and `cmake/OpenShowSkia.cmake`. Dependency setup, system
versions and old probe results are in the archive; package notices/build manifest
remain **7.3**. Do not use old machine paths as current dependency configuration.

## Task, pause and Git rules

1. Work in milestone/dependency order. Preserve recorded DONE checkpoints;
   later work does not waive earlier open tasks. 6.9b is DONE (2026-10-30,
   the prepare-time transport prime); the recorded extractions 6.18a1 and
   6.18a2 are DONE (the earlier "untracked
   preparation" note is closed out: 6.18a2 was implemented, wired and tested
   2026-10-29); 6.18b is split in the ledger (6.18b1 transport extraction and
   6.18b2 the dev-driver split, both DONE 2026-10-29). Finish/check boundaries without
   claiming scheduling completion.
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
8. Keep PLAN.md concise and stable. Update task statuses as work proceeds, but
   do not add or expand implementation details. Change work-item descriptions
   only when additional work items are needed or a section must be split into
   multiple work items; keep those additions brief. Record implementation notes,
   decisions and verification results in PROGRESS.md, architecture documentation
   or history as appropriate, without copying them into this plan.

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
| 1.5i | DONE (compatibility path) | Mapped LINEAR NV12/P010 staging images copied to OPTIMAL images and borrowed by Skia with a YCbCr sampler. VA-API transfer writes mapped staging memory but **still performs an FFmpeg CPU frame copy**; software decode copies/interleaves or converts. This path is now an explicitly permitted startup fallback (`--allow-video-copy`), not direct decode. | Original GPU sampler and staging tests retained; High10 software compatibility conversion tested. |
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
| 5.3 | DONE | Stereo mixing with independent source gain/mute | `audio_mix_tests`; single-stream bench differs from production (see architecture audit) |
| 5.3a | DONE | Bounded per-input rings and clickless gain ramps | `audio_mix_tests` |
| 5.3b | DONE | Mix through real file feeders | `audio_mix_tests`; analytical RMS and starvation checks |
| 5.3c | DONE | Preview-enabled live two-source PipeWire bench | Historical zero-underrun run; daemon-dependent acceptance |
| 5.4 | DONE | AvPlayer transport and playback-clock orchestration | `av_sync_tests`; daemon-dependent cases report skips |
| 5.4a | DONE | Inactive sink start, pause/resume and clock anchoring | `av_sync_tests` |
| 5.4b | DONE | Audio seek flush, rebase and acknowledgement seam | `av_sync_tests`; failures/generation pairing refined in 6.7d |
| 5.4c | DONE | AvPlayer pre-roll, lifecycle and clock fallback | `av_sync_tests`; restartability refined in 6.7a/b |
| 5.4d | DONE | Clock, feeder and complete transport regressions | `av_sync_tests`; thirty-minute hardware gate remains M7 |
| 5.5 | DONE | Prepared assets, continuous video backgrounds and failures | `background_media_tests`; the transport-side scheduling limit this row named is closed by 6.9b |
| 5.5a | DONE | Video background identity in compiled scenes | `scene_tests` |
| 5.5b | DONE | Production background transport and lyric continuity | `background_media_tests` |
| 5.5c | DONE | Prepare-time still decode and video probe | `background_media_tests`; since 6.9a off the RPC thread on the prepare executor, ack deferred to readiness |
| 5.5d | DONE | Sticky acknowledged media errors and notifications | `background_media_tests`, `protocol_tests` |
| 5.5e | DONE | Live continuity, media-swap and preload acceptance | Historical p95 18.7 ms; fresh target-machine gates remain M7 |

Checkpoint: `M5: integrate synchronized video and audio playback`.

### M6 — Code review remediation and architecture

The [M6 review](docs/M6-code-review.md) introduced R1–R19; **6.0** completed
the review only. Productization moved to **M7**; old 6.1–6.4 reliability
references in historical logs/source mean 7.1–7.4. M6 remains incomplete.

| ID | Status | Focused work | Acceptance / review finding |
|---|---|---|---|
| 6.0 | DONE | Architecture and RPC review; focused remediation ledger | [Original R1–R19 findings](docs/M6-code-review.md); source audit in [architecture.md](docs/architecture.md) |
| 6.1a | DONE | Black production startup; aux/clock/diagnostics off by default | `production_output_tests`, `render_production_compositor`; R1 |
| 6.1b | DONE | Production SceneRenderer; opt-in demo decoration/harness | Production/pixel tests; CLI diagnostics remain linked into worker; R1 |
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
| 6.7c | DONE | Typed decoder, feeder, sink and background failure state | Media/AV/background fixtures; foreground item reporting still incomplete; R18 |
| 6.7d | DONE | Paired seek generations, failure reports and shared deadlines | `playback_control_tests`, `av_sync_tests`, `background_media_tests`; R19 |
| 6.8a | DONE | Encoded-size, scene/reference and JSON-RPC envelope limits | `protocol_tests`, `scene_tests`, `worker_rpc_limits`; R11 |
| 6.8b | DONE | Bounded nonblocking RPC writers and stall handling | `outbound_queue_tests`, `worker_rpc_transport`, `worker_controller_tests`; R11 |
| 6.9a | DONE | Move file decode/probe preparation off the single RPC loop into a bounded cancellable executor | `prepare_executor_tests` (exactly-once/capacity/cancel/stop contract), `background_media_tests` (live: status/displays/release/shutdown-class probes answered during four cold 4K prepares, `scene.preparing` evidence, `preloadHits` cache proof); `PrepareExecutor` joins `SceneStore`/`PreviewProducer`/`BackgroundTransport` as session-owned leaves. R10 |
| 6.9b | DONE | Prepare bounded media/text/blur resources before go; move transport start/pre-roll/retirement waits off the render loop, preserving Ganesh ownership | `background_media_tests` (live: a scripted 700 ms video open is a PRIME on the prepare executor — its ack lands ~1.1 s later while the RPC answers status in ≤0.3 ms and the audience's frames advance with a 50 ms worst gap; presenting the primed clip reaches the air in ~15 ms, a same-identity lyric change in ~2 ms, warm switches p95 ~9 ms over 15; the draw-boundary trio — decodes, blur backdrops, text layouts — stays +0 across the warm loop; one prime parked at a time, a superseded one closed on the priming thread; shutdown answered in 0.1 ms with the queue full), `scene_frame_tests` (a companion's layouts stay its own: R16). Staged transport open (`PlaybackPipeline::openFile` on any thread / `attachGpu` on the render thread), `BackgroundTransport::primeVideo`, per-`SceneRenderer` `LayoutCache`, prepare-time blur prewarm. R10 |
| 6.10 | DONE | Budgeted image cache, input limits, pins and retry | `scene_image_cache_tests`, `background_media_tests`; pinned excess reported, not evicted; R12 |
| 6.11a | DONE | One immutable audience frame; previews cannot advance fades | `scene_frame_tests`, `preview_budget_tests`; synchronous readback remains; R13 |
| 6.11b | DONE | Correct background fade groups without intermediate targets | `scene_render_tests`, `render_layer_guard` and negative fixtures; R13 |
| 6.12 | DONE | Adopt saved/recovered assets across model/history/scene references | Domain/controller regressions; R7 |
| 6.13a | DONE | Staged durable asset/manifest commit and explicit I/O failures | Domain interruption/fault regressions; complete bytes guaranteed, strict version rollback not promised; R8 |
| 6.13b | DONE | Loaded ID validation and symlink-aware asset containment | `domain_tests`; R8 |
| 6.14a | DONE | Visible autosave/clear failures; flush edits on exit | `document_controller_tests`; R14 |
| 6.14b | DONE | Bounded coalesced background autosave and reusable asset copies | Domain/controller regressions; explicit save/load still synchronous; R14 |
| 6.15a | DONE | Typed production playback, seek, loop, gain and mute controls | Protocol/controller/background regressions; R15 |
| 6.15b | DONE | Standalone track selection with independent transport lifetime | `background_media_tests`; two production PipeWire streams need reconciliation; R15 |
| 6.16a | DONE | Worker-rendered selected-scene editor preview | `preview_selection_tests`, `worker_controller_tests`; R16 |
| 6.16b | DONE | Bounded worker thumbnails over preview transport | `preview_selection_tests`, `ui_loads`; R16 |
| 6.17a1 | DONE | Stable multi-text-box model, storage and compiler | Domain/scene/controller regressions; legacy migration; R17 |
| 6.17a2 | DONE | Editor text-box selection, creation, geometry and ordering | `document_controller_tests`, `scene_render_tests`, `ui_loads`; R17 |
| 6.17b1 | DONE | Foreground images in model, storage, wire and ordered draw | Domain/scene/pixel regressions; mixed-item layer controls need reconciliation; R17 |
| 6.17b2 | DONE | Editor image creation, asset choice, geometry and ordering | `document_controller_tests`, `scene_render_tests`, `ui_loads`; R17 |
| 6.17c1 | DONE | Cover/contain background fits end to end | Domain/scene/pixel regressions; unknown fit names refused; R17 |
| 6.17c2 | DONE | Cached blur-fit still backdrop; refuse blur on video | `scene_image_cache_tests`, `scene_render_tests`; cold build still in draw path; R17 |
| 6.17c3 | DONE | Vertical background gradient through model/storage/render/editor | Domain/scene/controller/pixel regressions; gamma-encoded sRGB interpolation; R17 |
| 6.18a1 | DONE | SceneStore owns prepared scenes and desired/landed/published state | `scene_store_tests`, `output_status_tests`; immutable sharing optimized 2026-10-29; R1/R4/R5/R10/R13 |
| 6.18a2 | DONE | Extract `PreviewProducer` from `OutputSession` (second half of 6.18a) | Stream lifecycle, selection state, thumbnail queue/decode policy and the audience-budget policy now have one owner beside `SceneStore`; the session keeps the handoff (frames in, slots out) and the wake. `preview_producer_tests` (headless) plus unchanged live gates `preview_stream/ownership/selection/budget_tests`, `worker_rpc_lifecycle`, `production_output_tests`; 29/29 headless and 50/50 desktop 2026-10-29. R5/R11/R13 |
| 6.18b1 | DONE | Extract the background transport from `OutputSession` into `BackgroundTransport` (first half of 6.18b) | Background pipeline + audio feeder, standalone track and its swap, the video seam's bounded slots and their leases, the intent slots, the typed faults and the whole media status mirror now have one owner (`session/BackgroundTransport.h` states the R6 quiesce rule and the threading); the session keeps the loop's handoffs (`submissionWake`, `takeDueVideo`, `applyBackground` in, started-out) and the delegating RPC/bench verbs, `status()` reads the transport's mirror, and production and bench drive the same object. `background_transport_tests` (headless) plus unchanged live gates `background_media_tests`, `production_output_tests`, `worker_rpc_lifecycle`; 30/30 headless and 50/50 desktop 2026-10-29. R6/R10/R15 |
| 6.18b2 | DONE | Move the bench/diagnostic drivers out of worker main into a dedicated dev target | `openshow-worker-dev` (`dev/main.cpp` + `render/TestScene.cpp`, compiled with the same `OutputSession.cpp`/output sources as the product binary) now owns `--render-test`/`--idle-check`, `--snapshot`/`--scene-file`, `--check-layers`/`--check-transition`/`--check-production`, `--pw-check`, `--av-bench` and `--list-monitors`; the product `openshow-worker` is socket mode only (`--socket` plus `--monitor`/`--seconds`/`--vulkan-validation` and `--version`/`--check-deps`, the latter one shared body in `DependencyReport.h`), and the demo scene and pixel checks are no longer reachable from it. The five CLI-mode gates (`render_scene_snapshot`, `render_layer_composition`, `render_scene_transition`, `render_production_compositor`, `render_idle_check`) run the dev binary; `worker_check_deps` still proves the product binary; `worker_dev_is_qt_free` guards the new one beside `worker_is_qt_free`. 31/31 headless and 52/52 desktop 2026-10-29. R10 |
| 6.19a1 | DONE | Extract `RpcClient` from `WorkerController` (first component of 6.19a) | The socket, the shared framing, the reply ledger, the send bounds and the write-stall deadline now have one owner (`src/ui/RpcClient.{h,cpp}`, `openshow-ui-core`), and it decides nothing about the application: a reply is handed to its requester with the method its request was sent under, the connection generation it was written on and the caller's own intent record, so R3's correlation rule lives in one place and adding a protocol method cannot touch it. `WorkerController` keeps the process, the handshake/poll rhythm, the acknowledged output model and the preview pictures, and tells the transport when to stop reaching for the worker. `worker_controller_tests` (the R3/R5/R11 request-ownership fixtures, unchanged: the in-flight and backlog refusals, the 2 s no-byte deadline and the slow-worker exemption) still drives the real controller through the new seam; 53/53 desktop (244.9 s, 12 display) and 32/32 headless (51.7 s) 2026-10-30. R3/R5/R11 |
| 6.19a2 | DONE | Extract `PreviewConsumer` from `WorkerController` (second component of 6.19a) | The shm mapping named by `preview.start` and everything that decides what may be painted out of it now have one owner (`src/ui/PreviewConsumer.{h,cpp}`, `openshow-ui-core`): the audience mirror, the editor's off-air target and the slide list's band with their two independent generation counters, asked/wanted dedupe and frame gates, the byte-bounded thumbnail LRU (count and bytes, a picture dropped when its slide's text changes) and the status-poll stand-downs that re-issue an ask the worker no longer holds. It sends through callbacks its host injects — request, notification and the same 6.8a scene-budget predicate a present passes, so that rule keeps one owner — and interprets no reply but its own methods' acks, the `preview.ready` frame and the `preview` block of `output.status`; `scene.prepare`'s intent, which both preview asks ride on and the session's mirror write shares, moved to the shared `src/ui/SceneAsk.h` so the two classes name one record. `WorkerController` keeps the process, the handshake and poll rhythm, the acknowledged output model and the QML surface (its preview getters and `previewChanged`/`thumbnailReady` forward), and lost 565 lines of `.cpp` and 118 of header. `worker_controller_tests` (its preview and thumbnail scenarios, unchanged) plus live `preview_selection_tests`, `preview_stream/ownership_tests` and `preview_budget_tests` retain behaviour. R5/R11/R16 |
| 6.19a3 | DONE | Extract `PresentationModel` from `WorkerController` (third component of 6.19a) | The scene-revision mirror and what derives from it now have one owner (`src/ui/PresentationModel.{h,cpp}`, `openshow-ui-core`): the prepare payloads by revision with the bounded window, the latest-go rule, clear derivation from the pending logical presentation and the restore snapshot with its confirmed-on-air retirement, `sceneContent`, the asset-path adoption across mirror, in-flight prepares and owed scene, and the desired/confirmed pairing — so a QML property reads exactly one truth, and the facade's `sceneActive`/`scenePending`/`scenePrepared`/`wordsOnAir`/`backgroundOnAir`/`canRestore` and its go/clear/restore/adopt invokables forward. Like `PreviewConsumer` it owns no transport: it sends through injected callbacks (live gate, the same 6.8a budget predicate, request, the prepare intent it arms after the send, the pending-prepare visit and the status line), interprets only the prepare ack, the `scene` block of a status answer and a refusal, and the QML surface is byte-identical. `worker_controller_tests` (the R3/R5 request-ownership and go/clear/restore scenarios, unchanged) plus live `document_present_tests` and `ui_loads` retain behaviour; `WorkerController.cpp` lost 247 lines and its header 34 more. R3/R5/R16 |
| 6.19a4 | DONE | Split worker `RpcServer`'s connection loop from its method and status codecs (fourth component of 6.19a) | `RpcServer` is now the connection and nothing else (`run`/`serve`: accept, framed read, the hello gate, the bounded queue and its progress-measured write deadline, the drain, the deferred-answer bag with its self-wake, shutdown), and `src/worker/rpc/RpcMethods.{h,cpp}` is the meaning of a request: the method table (every argument read in its own width with the R11 range rules, the per-method refusal texts, the result encodes, the session calls and the deferred `scene.prepare` ack handed back through an injected sink) plus the status and notification encoders (`output.status`, `output.displays`, `preview.ready`, `media.error`) so what the frontend decodes has one worker-side author. Operation names and result shapes stayed the shared typed definitions in `openshow/protocol` — the split added no wire vocabulary. Only the connection can end a session; the table never will. `RpcServer.cpp` lost 909 lines. `worker_rpc_lifecycle` (5.6 s), `worker_rpc_transport` (13.6 s), `worker_rpc_limits` (12.1 s) and `output_status_tests` (3.7 s), all unchanged, retain behaviour. R3/R11 |
| 6.19b1 | DONE | Extract the save/autosave adapter from `DocumentController` (first component of 6.19b) | Durability now has one owner (`src/ui/DocumentPersistence.{h,cpp}`, `openshow-ui-core`): the `AutosaveScheduler` and its thread, the 2.3 startup rule that a scratch is newer than the saved service and so wins, the deferred refusal surfacing that keeps the crash net while making the refusal the last word (with the stale flag suppressing a warning the scratch has already made untrue), the decision-epoch quiesce/resume protocol an explicit save or load runs, the scratch removal with the survivor wording a silent resurrection would cost, the quit-edge durable wait and the executor counters. `DocumentController` stays the one `Documents`/`History` owner: a save hands the rebinds back for `History::adoptAssets` and decides navigation and the dirty flag; a load receives separate documents and moves them in when it is ready to give up what it had; the adapter holds neither and reaches the operator only through the injected status line. `document_controller_tests` (the 6.14a/6.14b fixtures, unchanged: the coalescing counters, the decision-epoch veto, both survivor status texts, the recovery-over-service startup, the aboutToQuit flush), `document_present_tests` (1.0 s) and `ui_loads` (8.0 s) retain behaviour; `DocumentController.cpp` lost 116 lines and its header 8. R7/R14 |
| 6.19b2 | DONE | Extract the list projections from `DocumentController` (second component of 6.19b) | Row shapes now have one author (`src/ui/DocumentViews.{h,cpp}`, `openshow-ui-core`): the song rows, a song's slide rows with the first box's geometry and background, the current slide's box rows, the style list and the resolved style with its gradient, the playlists and their entries with each referenced show resolved, the media registry, the drawer's search projection, and the selected box's text, kind and asset. It is a pure reader — const reference to the controller's `Documents`, no state, no signals, no cache — and where a shape depends on where the operator is, the navigation arrives as a `Cursor` value the controller builds from the ids it owns; `selectedBox()` is the single resolution of the selection with its fallback to the bottom of the stack, and the controller's own box-scoped edits resolve through that same call (R17's highlighted-box-is-the-edited-box rule stays one function). The controller keeps the documents, the navigation and the edits, and every property getter forwards; `commitPendingText` compares its draft against the views' own `linesAsText`, so the inspector's text and the debounce cannot disagree. `document_controller_tests` (the navigation, text-debounce, box-selection/undo-follow, style and playlist scenarios), `library_search_tests`, `workspace_panels_tests`, `scene_edit_tests`, `document_present_tests` and `ui_loads`, unchanged; `DocumentController.cpp` lost 261 lines. R16/R17 |
| 6.19b3 | DONE | Move the window's keyboard shortcuts from `Main.qml` into `ShortcutMap.qml` (third component of 6.19b) | The whole key table is one component (`src/ui/ShortcutMap.qml`, 388 lines): all 49 `Shortcut` objects — output Escape/F1/F2, the Ctrl+D and Ctrl+F drawer rules, Ctrl+1..5, the nudges and stack steps, presenter navigation and number keys, the document keys and F11 — with the map computing its own gates (`nudging`, `boxStepping`, `presenterNav`, `numberKeys`) from eleven explicit fact properties passed IN (page, what holds the focus, the canvas's handles, the on-air state, the settings opt-in), and every action emitted as a signal the window handles beside the same buttons that perform it. No key changed owner, sequence or gate: the two Escape owners still hand off on `boxSelected`, the Ctrl+D pair still splits on slide count, the presenter arrows still stand down for `nudging`, and R17's one-enabled-owner rule — Qt drops a key two enabled `Shortcut`s claim — now reads in one file instead of across the window. Verified live: the shell loads with the identical notice set, the three multi-sequence `StandardKey` notices having travelled with their shortcuts into `ShortcutMap.qml`; `ui_loads`, `ui_links_qt`, both `ui_loads_guard_*` negatives and `document_present_tests`, unchanged. `Main.qml` lost 240 lines. R17 |
| 6.19b4 | DONE | Move the right dock from `Main.qml` into `OutputPanel.qml` (fourth component of 6.19b) | The preview and the output tools are one component (`src/ui/OutputPanel.qml`, 378 lines): the `SidePanel` it grows out of, the shared-memory preview canvas and its frame/drop or error readout, the monitor picker bound to the worker's applied display, the five layer toggles, the demo-scene row, the per-layer clears and the Clear-all/Restore toggle (each gated on the worker's acknowledged live flags, never a click), the media transport and mixer rows with their at-rest-only slider re-application, the standalone track row, and the device/frame/revision line. Every fact it shows mirrors `worker` and every drag reports to `panels` — the window's remaining ownership is placement: the caller anchors it between top bar and drawer and keeps the `rightDock` id the two centre pages anchor to. The window's private inline `PanelField`/`PanelButton` became module files (one shared look, not a private declaration the extracted panels cannot reach), and the moved `SidePanel` signals stay internal because their handlers were always `panels` calls. `ui_loads` (live notice set re-verified: the same five — the three multi-sequence `Shortcut` notices and both `Flow` anchor complaints, the latter now naming `OutputPanel.qml`), its two guard negatives, `ui_links_qt` and live `document_present_tests`, unchanged; `Main.qml` lost 367 lines. R13/R15/R16 |
| 6.19b5 | DONE | Move the left dock from `Main.qml` into `PlaylistPanel.qml` (fifth component of 6.19b) | The running order and its gestures are one component (`src/ui/PlaylistPanel.qml`, 294 lines): the playlist chips with the current one highlighted, the new-playlist `+`, the entry rows with each one's section, resolved show name and current-song highlight, the ▲/▼/✕ reorder handles that make the reference's drag move without a mouse, the row drag itself (`Drag.mimeData` carrying `entry:<id>`), the drop target that lands in the gap nearest the pointer — including the after-the-move `toIndex` shift for a row moving downwards — the empty "Nothing here" row, and the status line with the unsaved marker over the service directory. Like OutputPanel it needs no properties and no signals once read honestly (the row asked for signals for insert/remove/move/select; every gesture addresses `docs`, the context property, and the rows are the 6.19b2 projections), so the window keeps the anchors and the `leftDock` id the centre pages use. `ui_loads` (live notice set re-verified: the same five, and the dock emits none of its own), its guard negatives, `workspace_panels_tests` and `document_controller_tests`, unchanged; `Main.qml` lost 270 lines. R16 |
| 6.19b6 | DONE | Move the show page from `Main.qml` into `ShowPage.qml` (sixth component of 6.19b) | The slide list and the lyric editor are one component (`src/ui/ShowPage.qml`, 342 lines): the Add/Remove actions and slide counter, the band logic (current ±5, one ask after the handshake latch, re-ask on navigation/documents/text changes) that gets each row its worker-drawn `PreviewView` picture beside the legible text line, the Undo/Redo/Save/Load row, Prev/Next/Present with the follow checkbox, the picture panel (objectNames kept) that replaces the field when a picture is selected, and the lyric `TextArea` with its binding-re-establishing `Connections`. The row asked for select/step/present/text intents; read honestly only ONE has a window-side owner — selecting, stepping and editing address `docs` directly, and follow mode (`following`, task 3.3) travelled with its only consumer, so the page owns it and emits just `presentRequested()`, which the window performs with the `presentCurrentSlide()` the keyboard executors share. `ui_loads` (live notice set re-verified: the same five), `document_controller_tests`' text-debounce and navigation scenarios (3.5 s — the debounce lives behind the `setSlideText` seam the page calls), and live `document_present_tests`, unchanged; `Main.qml` lost 314 lines. R14/R16 |
| 6.19b7 | DONE | Move the edit page from `Main.qml` into `EditPage.qml` (seventh component of 6.19b) | The stage and its chrome are one component (`src/ui/EditPage.qml`, 646 lines): the `EditCanvas` with its outlines, handles and gestures, the debounced ask for the worker's picture of the slide under edit (6.16a — one ask per quiet moment, payload compiled at the tick, the target stood down when the page is not visible), the exact X/Y/W/H fields that commit all four on focus-out and snap back as a unit on a refusal, the stack commands whose enabled state reads the session's selection, and the style panel — combo, New/Show-default/Follow-show, colours, the gradient field that says flat by being empty, font/size/align/line-spacing, background asset and fit. Fourth honest correction to the split's rows: geometry and stack edits address `docs` directly, and the nudge/step executors travelled WITH the canvas because only it tracks the live rectangle a nudge commits from — the page exposes `boxSelected` (the alias the key map's two Escape owners hand off on, and Escape's deselect lands through) plus `nudgeBox()`/`stepBox()` for the key map's handlers, and the window keeps placement and the page gate. The selection stays the controller's one resolution (R17: the box wearing the handles is the box the fields and arrows edit). `ui_loads` (live notice set re-verified: the same five), `document_controller_tests`' box scenarios (selection following identity, geometry one-undo-either-way, the refusal snap), `scene_edit_tests` and live `document_present_tests`, unchanged; `Main.qml` lost 612 lines and is now 389. R17 |
| 6.19b8 | DONE | Move the settings page from `Main.qml` into `SettingsPage.qml` (eighth and last component of 6.19b) | The settings page is one component (`src/ui/SettingsPage.qml`, 168 lines): the two persisted toggles over `appSettings` (number-keys presenting, F11 fullscreen), the cheat sheet of the fifteen rows the keyboard flows rely on, and the service rows — version with the worker's status, the controller's service directory, the autosave explanation. The row required the cheat sheet to state the keys `ShortcutMap.qml` now owns: it was checked entry by entry against the map (all fifteen match their owners and sequences), the page header now names the mirror duty ("ShortcutMap.qml is where a key changes; this list is the promise the operator was given — change both"), and `ShortcutMap.qml`'s header gained the matching pointer — which moved its three live multi-sequence notices from lines 92/289/304 to 96/293/308, the same three, still the whole extra output. No properties, no signals: all four reads are context properties, the fifth honest finding of its kind. `ui_loads`, `ui_links_qt` and `workspace_panels_tests`, unchanged; `Main.qml` lost 134 lines and is now 255 — placement, the focus facts, the executors, and the key map's intents landing beside them. With this leaf the 6.19b split is complete: every component the split named has its own file. R17 |
| 6.20a | DONE | Build the R1–R19 evidence matrix (first component of the 6.20 split) | The closure table is in `docs/architecture.md` ("Review closure: R1–R19 evidence matrix"), one row per finding: the remediation tasks that answered it, the named fixtures that pin it marked **H**/`D`/**D✦** by the preset inventory that actually registers them (32 headless, 53 desktop, 12 display-labelled), and the open remainder where one exists. Every fixture named in the matrix exists in its stated preset's inventory and passed this leaf's recorded gate — desktop 53/53 and headless 32/32 on 2026-10-30, both trees unchanged so the run is the same code the matrix describes. Bare findings are recorded bare with the leaf or milestone that owes them: the images-layer z order and the audio-contract wording to **6.20d**; per-item foreground reporting to **6.20d**; restart reset, device-loss classification and unbounded waits to **M7.1a–c** exactly as the review mapped them; redraw accounting, signed drift, pinned-cache excess and readback cost to **M7.2**; the cheat-sheet alignment honestly marked as verified by reading, no automated keyboard test existing. The completion rule's running obligations are named at the foot of the table with their leaves: 6.20b (guard re-read, audit table), 6.20c (production proof), 6.20d (three decisions) — the matrix evidences R1–R19, M6 stays open until those land |
| 6.20b | DONE | Complete the public-Skia check and the bounded-RPC traffic audit (second component of the 6.20 split) | Re-read done, and it earned its keep. `CheckNoSkiaPrivate` scans by directory glob (`src`, `tests`; 144 files, every module added since 6.2a included automatically), its exemption list empty, the exported include paths already narrowed by 6.2a to `include`/`include/third_party/vulkan` — but the Skia **root** itself is exported too, because the modules' public headers are reached as `<modules/<m>/include/...>`, so `<src/...>` resolved at compile time while the guard matched only quoted includes. The guard now refuses both quote styles and the negative fixture trips on both (`violation_angle.cpp` added; positive green, negative exits 1 by construction). The review's RPC/media audit table in `docs/M6-code-review.md` now states the enforced numbers with a dated revision marker over the preserved route inventory: 1 MiB `kMaxMessageBytes` both directions (sender refuses locally, connection survives), parse depth 8 against the measured legitimate 5, scene budgets 8/256/1024/16 KiB/64 KiB/512 with `data:`-style refs refused as asset paths and the every-budget scene pinned at 39% of the frame, method names 64 B, echoed text clipped to 160 B at a UTF-8 boundary, outbound queue 1 MiB/1024 frames with stall deadline, client in-flight 64, preview negotiation 64..1920 × 48..1080 @ 1..60 validated before any mmap (UI asks 640×360@15), on-demand targets bounded separately — `preview.select` carries no geometry, thumbnails ≤ 32 × (128 B key, 32..512 px), each extent re-checked against the slot — and the still-true findings stated still-true: no media-byte or data-URI method exists on the wire, the surviving readback-on-render-thread limit stays an open-differences row with its cost measurement at **M7.2**. The R9/R11 matrix rows now point at what landed. One naming correction to the split's row: there is no `rpc_limits_tests` — the headless budget fixtures are `protocol_tests`, `scene_tests` and `outbound_queue_tests` beside the live `worker_rpc_limits`/`worker_rpc_transport`, and those are the ones this audit cites; all unchanged in behaviour, all green in this leaf's gates |
| 6.20c | TODO | Run the production author→preview→go→clear→restore→media-swap proof (third component of the 6.20 split) | One display-labelled fixture driving a real `--socket` production worker end to end: author a song and slides through the same `DocumentController` the panels bind, compile with the same calls the panels make, ask the off-air picture, present to the audience, clear per layer and all, restore, then swap the background asset out from under the still-live words — each hand-off asserted through worker status and the preview stream, no dev-harness substitution. Where the run cannot use the real device or live PipeWire, it records that in its own output as the honest skip it is |
| 6.20d | TODO | Resolve the layer-control, audio-contract and foreground-error differences (fourth component of the 6.20 split) | The three differences PLAN and the audit name by hand: the `images` layer toggling only development decoration while production foreground images ride the words stack under mixed z order; the contract's one-software-mixed-stream wording against the two independent transports/streams that shipped (R2's restartable mixer included); and the missing-foreground-image failure that is logged but not reported per item the way the acknowledged media lane reports. Each is resolved as the decision, not the prose — code changed to match the contract, or the contract rewritten with the reasoning and an explicit M7 gate — and "not hidden by a documentation rewrite" is honoured by naming which of the two happened, where. If the honest resolution of any one turns out to be a code slice over a day, it is split again before it is started |
| 6.20e | TODO | Record the M6 closure and its honest skips (fifth and last component of the 6.20 split) | With the matrix, audit, proof and reconciliations in, the review's completion rule is discharged in writing: the final desktop/headless gate numbers, every skipped or environment-limited check listed at the point it was skipped (live PipeWire, RADV versus llvmpipe, hardware timing, projector placement, service soak), PLAN's M6 ledger and header and PROGRESS's handoff brought to the status the evidence supports, and no milestone claim stating more than the recorded runs prove |

Checkpoint: `M6: close code review and graduate production architecture`.

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
