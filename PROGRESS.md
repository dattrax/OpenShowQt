# OpenShowQt progress and session handoff

Updated: 2026-10-28 (Europe/London).

## Current checkpoint

- **M5 implementation checkpoint reached; M6 review complete (6.0 DONE).**
  The user requested a thorough review before productization. The authoritative
  review is [docs/M6-code-review.md](docs/M6-code-review.md); the new M6 leaf-task
  ledger is in [PLAN.md](PLAN.md#m6--code-review-remediation-and-architecture).
  M6 implementation tasks are all TODO. Productization is now **M7**; old
  6.1/6.2/6.3/6.4 reliability/diagnostics/package/rehearsal tasks became
  7.1a–c/7.2/7.3/7.4. Old source/log references retain their historical meaning.
- Review scope: production session/rendering, Skia build/API dependencies,
  protocol/router/frontend consumer, preview and GPU slot ownership, AV
  transport/decoder controls, document persistence/history/UI and test seams,
  compared with the fixed architecture and initial scope in PLAN.md. Reviewed
  `1f44b7e` plus the existing six-line clock log change in OutputSession.cpp.
- **19 findings** map to focused fixes with acceptance checks. Main risks:
  production still constructs TestScene and paints its clock/footer; rapid
  prepare replies share one pending payload; applied status is published before
  presentation; preview releases lack stream identity and exact ownership;
  displayed video slots are released before held redraws are finished; audio
  transport reuse does not reset the mixer; recovered assets are deleted while
  documents still reference them; blocking file/transport work and unbounded
  decoded-image caching violate the scheduling/resource intent. The report
  also records the remaining Unicode/private Skia, persistence, seek, preview,
  authoring and production media-control gaps.
- **RPC audit:** current UI→worker messages contain text/style/geometry, IDs,
  asset paths and controls. No original/decoded bitmap, video or audio bytes
  are sent. Worker→UI pixels are composed previews through three shared-memory
  slots; ready/release RPC messages contain metadata only. Sender limits,
  per-method budgets, nonblocking writes and ownership enforcement remain TODO.
- **Architecture direction:** retain the two processes and existing domain,
  scene, media/playback and output libraries. Extract production compositor,
  scene/command store, preview producer, background transport, correlated RPC
  client/consumer/supervisor, document/save coordination and QML pages around
  explicit ownership boundaries. No rewrite or arbitrary file-size target.
- Checks run on this review's working tree:
  - `CCACHE_DISABLE=1 cmake --build --preset headless -j 2`: PASS.
  - `ctest --preset headless --output-on-failure`: **16/16 PASS**, 25.78 s.
  - `CCACHE_DISABLE=1 cmake --build --preset desktop -j 2`: PASS.
  - `ctest --preset desktop -LE display --output-on-failure`: **21/21 PASS**,
    25.93 s, five display-labelled tests excluded.
  - Initial build failed only because ccache attempted to write its read-only
    cache; disabling it resolved that environment issue.
  - Vulkan coverage used **llvmpipe**; live PipeWire subscenario skipped because
    daemon access was denied. No physical display tests, audible playback,
    target-GPU timing or long service soak claimed.
  - Temporary standalone probes in `/tmp/openshow-m6-review` confirmed:
    mixer restart after stop/join refuses input/start; future preview release
    sequence frees an owned slot; appendFrame emits >1 MiB that Decoder rejects;
    recover→save→clear deletes the live source path and a second save fails.
    These are review probes, not committed regression fixes.
- Working changes at review time: PLAN.md milestone replacement, this handoff,
  and the new review document only; the pre-existing OutputSession.cpp clock
  log was committed after the review as
  `worker/session: task 5.5 - log which clock paces a background transport`.
- **6.1a DONE (2026-10-25).** `--socket` composes with the production
  compositor: uniform black start/clear-all including the footer band, clock
  and aux layers off, no diagnostics in the footer, and output/preview drawing
  one identical scene (`TestScene::Profile`, `OutputSession::Options::
  production`). Pinned by `--check-production` (CPU: black incl. footer,
  presented scene pixel-identical to the demo path, demo picture intact when
  asked for) and `production_output_tests` (live `--socket` preview stream:
  black startup incl. footer pixel-for-pixel, RPC layer defaults, scene
  colour/words with a diagnostics-free footer band, black clear-all);
  negative-checked — with the profile off the R1 assertions fail exactly as
  the review described. `rpc_ipc_tests`' preview scenario went scene-driven
  (its old premise was the demo picture). Gates desktop **28/28**, headless
  **17/17**.
- **6.1b DONE (2026-10-25).** TestScene graduated. `render/SceneRenderer.{h,cpp}`
  (production `openshow-worker-render` library) owns the five layers, the fade
  state machine and the output-rule composition; the `TestScene` class no
  longer exists. The 1.x picture is now an opt-in `DemoDecoration` built by
  `demoBuilder()` in `render/TestScene.{h,cpp}` — the development harness
  (demo assets, snapshot, `--check-layers`/`--check-transition`/
  `--check-production` pixel harnesses), which stays out of the library and
  is driven only by the worker CLI. `OutputSession` constructs `SceneRenderer`
  with `Options::demo` (socket: empty ⇒ production), and the footer status
  line is gated on `fScene->production()`. Behaviour-preserving split: the
  harnesses drive the real compositor and all pixel claims — layer regions,
  fade timing, production/dev identity, black start — pass unchanged.
  Gates desktop **28/28**, headless **17/17** (live display tests included).
- **6.2a DONE (2026-10-25).** The worker now owns its GPU memory allocator.
  `output/VulkanMemoryAdapter.{h,cpp}` implements the public
  `skgpu::VulkanMemoryAllocator` contract on the system header-only
  VulkanMemoryAllocator and is the single allocator wired into every Vulkan
  path (injected once in `VulkanContext::createDevice` — windowed output,
  headless GPU tests, preview alike; one adapter, one VMA TU). libskia exports
  its own `vma*` C symbols globally, so the adapter's TU compiles VMA with
  `VMA_IMPLEMENTATION` + `VMA_CALL_PRE static` + static/dynamic function
  loading off: the two copies can never bind to each other, and VMA's entry
  points resolve through the backend context's `fGetProc` with exact scope
  discipline — physical-device functions via instance GIPA, device functions
  via GDPA on the real device (this machine's loader returns raw ICD
  `GetImageMemoryRequirements2KHR`/`vkBindImageMemory2KHR` pointers from
  instance GIPA that null-jump when called with the loader VkDevice; the
  segfault in `gpu_yuv`/`gpu_decode` during this task pinned it). Private
  factory call gone from `VulkanContext.cpp`, `GpuTypesPriv.h` gone from
  `Swapchain.cpp`. Test colours are now specified independently in
  `tests/YuvBt601.h` (BT.601 Kr=0.299/Kb=0.114 8-bit narrow-range published
  coefficients) instead of asking Skia's private `SkYUVMath` for the sampler's
  own matrix. `cmake/OpenShowSkia.cmake` exports public include dirs only and
  pins Skia to `a94df1cdab…` (configure error on mismatch;
  `-DOPENSHOW_SKIA_REVISION=any` overrides). New ctest guard
  `skia_public_api_guard` (+ negative fixture) forbids private/src includes
  across src+tests; its sole exemption `SceneText.cpp` (SkTArray/SkUTF/
  SkUnicode_hardcoded, entangled with the homemade Unicode backend) retires
  with 6.2b, and the guard was proven non-vacuous by removing the exemption
  (flags exactly those three lines). GPU tests re-run green under
  `OPENSHOW_VULKAN_VALIDATION=1`. Gates desktop **30/30** (live display tests
  included), headless **19/19**.
- **6.2b DONE (2026-10-25).** Real Unicode, and R9's private-Skia ledger is
  closed. Skia rebuilt with `skia_use_system_icu = true` (the `_77` ICU
  references lived only in `libskunicode_icu`, so ninja recompiled exactly
  that module; bindings now match the system ICU 78). The worker links
  `libskunicode_icu.a` + `ICU::uc/i18n`. `SceneText.cpp` lost the homemade
  `HardCodedUnicode` (~180 lines), the `SkUnicodes::ICU::Make()` shim and
  its three private includes — the public module is the engine's provider
  and the guard's exemption list is now **empty**: zero private/src Skia
  includes across src+tests. New `unicode_text_tests` pins what the upgrade
  buys: combining-mark clusters, skin-tone/ZWJ/flag emoji as single
  graphemes, UAX #14 wrapping of unspaced CJK, real UBA bidi regions for
  mixed Hebrew/Latin (the stub answered "one level-0 region"), working
  bidi/line/grapheme break iterators; script-font availability skips only
  the affected layout fixture, visibly. All 30 prior fixtures unaffected.
  Gates desktop **31/31**, headless **20/20**.
- **6.2c DONE (2026-10-25).** Reproducible Skia. `third_party/skia` is now
  a git submodule pinned to `a94df1cdabb03d3ead2486413bc2999b500aa4f8`
  (`.gitmodules` holds only path + canonical URL; the `--reference` used to
  clone instantly lives in git alternates, unpushed). `scripts/build-skia.sh`
  is the single recipe: revision pre-check, `tools/git-sync-deps` **from the
  checkout root** (its emsdk hook resolves CWD-relative paths — a bare
  invocation from elsewhere fails), GN via `bin/gn`/`$GN`/system/`fetch-gn`,
  `gn gen` from the root with the validated args (Vulkan, no GL, bundled
  png/jpeg/freetype/zlib/expat, `skia_use_system_icu=true`), ninja for all
  five libraries; `--check` inspects without building. `OpenShowSkia.cmake`
  resolves `SKIA_ROOT` `-D`/env → submodule → legacy `../skia`, auto-builds a
  library-less submodule at configure time (`OPENSHOW_SKIA_AUTO_BUILD=OFF`
  opts out) and now hard-requires `libskunicode_icu.a`. Proven from a bare
  submodule: real network DEPS sync, `bin/fetch-gn`, 1211-target build,
  `_78`-bound ICU module, both presets reconfigured onto the submodule and
  fully recompiled. Gates desktop **31/31**, headless **20/20**. (The dev
  machine's own `build/` and `build-headless/` caches pick the submodule on
  their next configure; `-DSKIA_ROOT=/home/jim/skia` still selects the old
  tree, which stays as a manual fallback, not as the project default.)
- **6.3a DONE (2026-10-25).** Worker reply correlation (review R3).
  `WorkerController` no longer identifies replies by result shape: every
  request is recorded by id (`PendingRequest{method, prepareJson,
  autoPresent, connection}`) before it is written, and a reply acts only on
  the request its id names — unknown, duplicate or previous-connection
  replies are dropped. Each connect bumps the connection generation, resets
  the shared `proto::Decoder` (new `reset()`) and clears pending intents, so
  a dead stream's bytes or wishes cannot cross into the next one. Each
  prepare ack mirrors its own request's payload under the revision that ack
  named (the single pending-JSON slot is gone), and the **latest-go rule**
  holds: only the newest go's ack presents, so a late older ack can mirror
  but never regress the audience; error replies consume their intent. The
  supervisor moved into `openshow-ui-core` and `start()` honours a test-only
  `OPENSHOW_WORKER_PROGRAM` override, so the new `worker_controller_tests`
  drive the real class against this very binary re-executed as a scripted
  RPC peer (held burst acks, a 400 ms late ack, a rejection, a mid-flight
  death), with the child reporting exactly which presents arrived. All four
  scenarios pin the final slide/revision/mirror and the no-stale-auto-present
  rule; mutation (latest-go → old flag semantics) fails burst and slow
  scenarios, and building the fixtures flushed out — and fixed — a vacuous
  delayed-ack queue of their own. Gates desktop **32/32**, headless **20/20**.
- **6.3b DONE (2026-10-25).** Clear/restore serialization and content
  retention (review R3). Clears derive from the **pending logical
  presentation** (`clearSourceScene()` = newest go's payload, falling back
  to the confirmed mirror), so back-to-back words+background clears compose
  instead of the second resurrecting what the first removed; a clear that
  changes nothing coalesces to a no-op. The restore keeps the pre-clear
  scene as **content**, not a store revision: `restoreOutput()` re-prepares
  the retained payload, so the worker pruning its store (>16 prepares)
  cannot lose the scene; the snapshot retires only when a status poll
  confirms the restore's revision on air, and an error reply leaves the
  retry available while indicators stay honest. `worker_controller_tests`
  gained a pruned store in the scripted peer plus three scenarios —
  composed clears with exact present history, restore surviving a 20-prepare
  window overrun, and a refused restore staying retryable — all proven to
  bite via three mutations (confirmed-snapshot derivation, early slot
  consumption, missing retire-on-confirmation). Gates desktop **32/32**,
  headless **20/20** (headless re-run three times green after one flaky
  parallel run whose failure no 6.3b code path touches).
- **6.4 DONE (2026-10-25).** Honest publication of applied state (review
  R4). The run loop stages one immutable `FrameSnapshot` (revision, mask,
  scene id, transition kind, monitor, and a per-scene-id **content pin**
  so a slow apply cannot be evicted out from under the frame) under
  `fMutex` in the same critical section as the present decision; the
  frame draws exactly it and only its successful present **commits**
  `layerMask/monitor/scene.active/scene.transitioning/scene.pending/
  revision` — failed acquires/presents publish nothing, retry, and are
  counted in the new `presentFailures`. Lifecycle documented and pinned:
  queued (pending names the accepted command), transitioning (committed
  but still blending), confirmed (active + pending 0 + revision caught
  up; pending recomputed every commit, so it self-heals). Preview frames
  are tagged with the revision they drew, never the latest desired one.
  Fault knobs (`OPENSHOW_FAULT_SCENE_APPLY_MS`,
  `OPENSHOW_FAULT_SCENE_PRESENT_FAILS` armed per scene change,
  `OPENSHOW_FAULT_HOLD_MS`) force the two windows R4 named;
  `output_status_tests` S1 (present failures: honest queued window, no
  early live indicator, exact failure count, correctly tagged first red
  frame) and S2 (command mid-apply: B never credited with C's revision,
  B's preview frame tagged B) are non-vacuous under four mutations
  (apply-time publication, pending-never-clears, latest-revision tags,
  latest-revision commit). present→air remeasured against honest status:
  p95 16.9–18.2 ms of 100 ms. The `background_media_tests` recovery wait
  now requires the transport's real head position — the old early-active
  lie had been starting that poll a transport-open early, and honest
  timing exposed the fixture's assumption (mediaPlaying's contract sets
  position −1.0 until the head frame). Gates desktop **33/33** (8-run
  stress green on the media fixture), headless **20/20**.
- **6.5 DONE (2026-10-26).** Preview slot ownership proven by stream
  identity and exact sequence (review R5). `PreviewStream::release` takes
  only the exact sequence the slot last published — the old watermark let
  R5's probe (`release(0, 102)` against sequence 2) hand the producer a
  frame a consumer still held; odd, stale, future, duplicate and
  out-of-range releases now free nothing, one release per publish ever.
  The segment name is the stream identity end to end: `preview.ready`
  carries `"stream"`, `preview.released` without it is refused
  (capability `preview.stream-id.v1`, protocol minor 4 — a stricter rule,
  not an additive one), `releasePreviewSlot` matches the live stream's
  name, `takePreviewReady` drops queued entries of retired streams, and
  `WorkerController` ignores foreign readys entirely (no copy, no
  release). `PreviewStream::create` validates geometry (nonzero, ≤4096,
  uint32 stride) before any mmap and resets the seq bookkeeping so a
  re-created stream starts a new ownership life. Headless
  `preview_stream_tests` drives the real object (split into
  `openshow::worker_preview`); live `preview_ownership_tests` holds all
  three slots over a real socket session and reads published/dropped as
  the meter: bogus releases of every class free nothing, the exact
  triple returns exactly one slot, and after a replacement the delayed
  old-stream releases — including the (slot, 2) triples the new stream
  opens with — free none of the new slots, with a socket-ordering mark
  past which no delivered ready may name the retired stream. Mutations:
  watermark restored fails the unit test and held-phase checks; name gate
  removed fails the foreign-stream and delayed-release checks exactly.
  (The stale-queue drop's own race window needs sub-frame timing the
  worker's drain path does not expose; the assertion covers it and the
  two release-rule mutations carry the fixture's proven weight.) Gates
  desktop **35/35**, headless **21/21**, 5-run stress green.
- **6.6a DONE (2026-10-26).** The displayed video frame is leased (review
  R6). The seam contract is now lease-based: pacing stamps the leasing
  decoder sequence immediately before submit; `acquireImage(token,
  sequence)` yields an image only while that exact generation leases the
  token — a stale held submission can never sample the slot's later
  content; `onPresented` is a per-flush GPU-completion obligation
  (`GpuYuvUploader::addSamplingDone`), not a release; only a sequence-proved
  `onReplaced`/`onDropped` ends the lease, and refusals count in
  `staleAcquires`/`staleReleases` while freeing nothing. The session gives
  up the replaced frame after the decision lock, relinquishes with its own
  proof when an acquire comes back null, and holds the graphics-queue lock
  across the preview-only acquire/draw/readback, so every draw that samples
  video is covered by the slot's obligations. New `gpuHeldFrameLease`
  fixture: a frozen clock holds one frame while the producer parks on its
  slot (published stops at 4, slot-waits climb), the held generation
  acquires one stable image with byte-identical readbacks across redraws,
  unproved acquire/release free nothing, and the proved handoff drains the
  clip. Five mutations (free-at-present restored, acquire check dropped,
  release never frees, release without proof, RR claim reverted) fail
  exactly the checks each should. It exposed, and this task fixed, a ring
  flaw: `FrameRing::claim()` always scanned from slot 0, so the lease
  protocol degenerated to two slots (wraps 4→2, decode cushion gone) —
  claim now round-robins from a cursor. Synchronization validation
  (`VK_LAYER_SETTINGS_PATH` with `khronos_validation.validate_sync`) is
  hazard-free on the gpu_yuv/gpu_decode/playback_queue suites and on the
  full `--av-bench --preview` session with `--vulkan-validation` (125
  presents, 75 preview publishes, drift max 1.8 ms). Gates desktop
  **35/35**, headless **21/21**.
- **6.6b DONE (2026-10-26).** The swap's generation boundary (review R6,
  second half). `teardownBackgroundTransport()` now runs
  `requestStop() + join()` **before** it touches the seam — presenter,
  clock, audio detach, pending/drawn — so `submitVideo()`'s unsynchronised
  presenter read on the pacing thread can never overlap a mutation; thread
  creation orders the next transport's publish and join orders the
  teardown, and `OutputSession.h` documents who may write `fVideoPresenter`
  and when. The start-failure path (verified: no thread is ever spawned
  before every `start()`/`startDecode()` failure point) additionally stops
  the audio player it had started — a failed transport start had been
  leaving PipeWire running until the next teardown. Deterministic
  regression: `submitQuiesceBoundary` in `playback_queue_tests` — six
  generations through a 25 ms slow sink (stop lands mid-submit as the
  norm), asserting join returns bounded, **no sink call is in progress the
  instant join returns** (sampled at join, not at close, so close's own
  teardown cannot mask an early return), nothing is delivered after
  join/close, joined generations stay frozen while a newer pipeline
  submits, and close()-alone (disconnect) drains identically; the
  mutation that detaches instead of joining segfaults the fixture every
  run. Session side, `background_media_tests` gained a swap-burst phase:
  eight A/B swaps at 120 ms spacing — each teardown races a young,
  submitting producer — after which the transport must settle on the final
  identity, playing and advancing, with zero media errors (3-run stress
  green). Gates desktop **35/35**, headless **21/21**.
- **6.7a DONE (2026-10-26).** The audio transport restarts whole (review
  R2, first half). `AudioMixer::reset()` returns a joined mixer to its
  pre-start state — inputs and their rings, per-input and run counters,
  and the `fStarted` gate `addInput()`/`start()` check — and is a no-op
  while the thread is joinable; `AvPlayer::stop()` resets the mixer
  instead of re-flagging only its own. `PipeWireSink::Impl::start()` runs
  idempotent `stop()` first and re-arms every readiness/activation/clock/
  stats field, killing the class where run 2 inherits run 1's STREAMING
  wait, skips its first-activation anchor behind a stale activeFlag, or
  reports yesterday's counters. The mixer-failure path in `start()`
  self-cleans (sink down, **mixer reset** — an orphan registered input
  would shift the retry's `addInput()` off its expected index and
  silently degrade it — flags re-armed), reachable from tests through the
  new `OPENSHOW_FAULT_AUDIO_MIXER_FAIL` knob. Also pinned: the sink's
  Format param arrives after the PAUSED readiness event for every
  generation — a fresh sink's rate is 0 the instant `start()` returns;
  the preferred-rate fallback and server-time re-latch cover it, and the
  fixture checks rate after the first callbacks. Fixtures:
  `restartLifecycle` (mixer level, 11 checks — generation 2's captured
  head is byte-exactly its OWN pre-written samples), `avPlayerRestart`
  (double-start, audio→audio, audio→silent→audio, visible silence then
  working retry, injected rejection→retry), `avPlayerRestartLive` and
  direct `sinkRestartLive` (generation 2's counters zero and stream
  inactive right after start — R2's exact residue). Mutations M-A..M-E
  (stop without reset, no sink re-arm, no failure-path self-clean, reset
  without flag re-arm, reset without clearing) each fail exactly their
  intended checks. Gates desktop **35/35**, headless **21/21**.
- **6.7b DONE (2026-10-26).** Stable source identity and a transport-
  obeying fallback clock (review R2, second half). `kVideoInput`/
  `kSecondInput` are now identities mapped through `fInputOf` to the
  mixer index registration handed each source: the standalone-only
  transport (only the second track wanted, landing at index 0) feeds,
  flushes, volumes, seeks and reports by name — the old rigid demand for
  index 1 silently started no feeder. The map re-arms with the mixer in
  `stop()` and the failure self-clean. The sinkless fallback clock
  (atomic base/anchor/hold) obeys the transport: `start()` anchors 0,
  `pause()` holds, `play()` continues (re-anchor before releasing the
  hold), `seekTo()` lands the target playing OR parked, and a failed
  feeder seek moves nothing. Fixing this surfaced a pre-existing defect:
  `--av-bench --no-audio` captures `clockFn()` without `start()`, and
  the un-anchored fallback read time since the steady_clock epoch — the
  bench paced by machine uptime (all 181 presents missed, drift +67 719
  s; identical on the 6.7a binary). The anchor now initializes in the
  constructor; the rerun: 101 presents, **0 missed**, drift mean +1.0 ms
  max 13.6 ms. Fixtures: `avPlayerStandaloneOnly` (identity end to end,
  plus restart), `avPlayerFallback`'s hold/continue/land/parked-seek
  checks (inverting 5.4's pinned untouched-by-seek — that was R2's
  complaint), `fallbackClockBeforeStart`. Mutations M-F/G/H/I each fail
  exactly their intended checks. Gates desktop **35/35**, headless
  **21/21**.
- **6.7c DONE (2026-10-26).** The media report is typed (review R18, first
  half). `AvPlayer` answers `audioLaneState()` from state it already keeps
  honest plus one new counter: `fSourcesOpen` — how many wanted sources
  ACTUALLY opened. That counter is the whole discovery: `fAudioLive` is
  `sinkUp`, so a live PipeWire sink over a file whose decoder never came up
  read "started" with silence and no field anywhere saying so. Precedence:
  nothing wanted → `none`; audio wanted and transport down → `degraded` (a
  failed start keeps the expectation deliberately — it is the only thing
  between this and a report of nothing-at-all; a clean `stop()` clears it);
  no source open → `degraded` (an asset fault outranks the machine's state,
  so a broken file can never be reported as "silent, output switched off");
  output off → `silent`; no server connection → `no-output`; paused →
  `paused`; live and active → `live`; else `degraded` (activation refused).
  Session: `status.media.audio` names the lane (protocol minor 5, capability
  `media.audio-state.v1`, UI `mediaAudio` + amber TopBar line for
  degraded/no-output); faults are typed (`MediaFault {image,video,audio}` +
  the path) so `mediaFaultCleared*` erases only its own class *and* asset,
  and `fExpectedAudioLoss` carries the one thing `AvPlayer` cannot know (the
  container has an audio stream, no decoder opened) through the per-frame
  reconcile. Two honesty fixes fell out: `playing` no longer credits at
  open (promoted on `submitted > 0`, demoted on `fatalError`/`eof`, with
  eof-while-looping reported as a video fault rather than silence), and
  `housekeep()` reconciles under `fMutex` every 500 ms before touching the
  graphics-queue lock — the run loop is event-driven and presents NOTHING
  once frames stop, so a frame-refreshed status would have claimed playback
  forever after a mid-play death. `SceneImageCache::refreshFor()` always
  decodes a cached failure (an operator action, unlike the 60 Hz draw path
  that keeps negative caching) and states its reason; `media.path` always
  names the on-air background asset, error set only when that asset is what
  cannot be shown; `VideoFrameSource::run()` no longer rewinds into its own
  end-of-stream when nothing was published since the last one. Fixtures:
  `audioLaneOfflineStates`/`audioLaneLiveStates` (six wire names pinned;
  no-output reached by cutting `XDG_RUNTIME_DIR` away so it reproduces on a
  box that HAS a daemon; live: live→paused→live, sink-up-with-broken-source
  →degraded, stop→none) and five new `background_media_tests` scenarios —
  `exerciseImageRepair` (missing still names itself and its class, the file
  returns under the same slide → repaired, error cleared, **no** new
  notification), `exerciseMidPlayFailure` (`::truncate(...,0)` mid-play →
  stops being reported as playing within 4 s, notification names the file,
  exactly ONE notification across 1.2 s of heartbeats, healthy clip heals
  it), `exerciseAudioLane` (anullsrc clip: never `none`, lane and error
  self-consistent whichever way the server came up), `exerciseFaultsDoNot-
  CrossAssets` (a queued slide's image fault stands through the healthy
  on-air transport's continuous reconcile), `exerciseAudioStartFailure`
  (own faulted worker: muted picture plays, lane `degraded` + pushed
  message, a silent clip clears it). Mutations m1–m7 (no heartbeat,
  `refreshFor` reusing the cached failure, lane ignoring `fSourcesOpen`,
  lane ignoring a failed start, untyped clear, lane ignoring sink
  readiness, `playing` at open) are each CAUGHT. Accepted limits: one
  sticky error string, so simultaneous faults share text; a server
  self-deactivating our stream is not observed; the "stream present, no
  decoder" branch has no dedicated fixture (no encodable-but-undecodable
  audio codec in this FFmpeg 9.0.2 build), and a media-level fixture for the
  progress guard was reverted because FFmpeg legitimately loops a truncated
  mp4 from its packet cache (755 frames, 107 loops). Gates desktop
  **35/35**, headless **21/21** (background_media 3-run, av_sync 2-run
  stress green).
- **6.7d DONE (2026-10-26).** Seek pairing, failed-seek reporting and the two
  time domains (review R19). A seek is a request a DECODE thread performs
  later, so the protocol is a pair: `media/SeekQueue` hands the applier
  `(generation, target)` from `pending()` and only `apply(gen)`/`refuse(gen)`
  for that exact generation settles it. Both sources used to read the newest
  target and then credit the newest COUNT, which settles a request that
  arrived mid-apply without ever moving the container to its target: the
  pacer reads `requested != applied` as "flush in progress", so it stopped
  draining and presented the superseded position while the caller was told
  the jump landed. Audio answers through `waitApplied(gen, 500 ms, abort)`,
  which distinguishes APPLIED from REFUSED — the distinction R19 asked for:
  a refused jump flushes nothing (nothing in the queue is stale), counts
  `Stats.seekFailures`, leaves the applied watermark where it was, keeps the
  heard position where it was, and surfaces as `MediaFault::kSync` naming the
  refused target; a video seek the container cannot perform stays FATAL (it
  refuses its generation on the way out so nothing can wait on it). The two
  time domains are typed rather than remembered: `VideoSubmission` carries
  `ptsSeconds` (media position) and `deadlineSeconds` (present time in the
  transport clock) and the session presents through one inline gate,
  `videoSubmissionDue(sub, clock)` — comparing the clock against pts is the
  bug R19 names, and it disagrees by a whole clip length after a wrap.
  Transport control reached the session (`requestMediaSeek/Pause/Play`, all
  applied on the render thread by `applyTransportOps()`, scripted in tests by
  `OPENSHOW_TEST_TRANSPORT_OPS`); exposing them over RPC is 6.15a's row, so
  6.7d lands no protocol change. **Pinned semantic:** a seek issued while
  held SCRUBS — the parked transport presents the target, the position moves,
  `playing` stays false, then it holds again. The clock a held transport
  paces against is frozen (so a new frame's only honest deadline is "now"),
  showing what you seek to is the entire point of a parked scrub bar, and the
  offer is the seam's own way out of the hold: it is what releases a
  presentation slot a producer may be waiting on, so a parked transport can
  always digest a seek instead of stopping.
  **Three production defects, all found by fixtures rather than reading:**
  (1) a GPU-mode DEADLOCK — with every presentation slot leased (the session
  holds two, ring depth == uploader `kSlots` == 4), the decode thread's next
  `commit()` parked on a slot that only the NEXT OFFER would free, and a seek
  is precisely what stops the pacer offering: it waited behind the very seek
  it was on its way to perform and the audience kept the last picture with
  the clock running. Fixed in three parts — `returnFrame(frame, submitted)`
  releases the uploader slot for a frame the pacer drops (nothing downstream
  can ever do it: `onReplaced`/`onDropped` never see a frame that was never
  submitted), `FrameRing::discardReady()` calls `discardCommitted(slot)` for
  each frame a seek flush purges, and `VideoFrameSource::createRing` wires the
  source's own stale-frame flag into ANY backing (`setProducerAbort`), so a
  consumer cannot forget it — the abort is raised by `requestSeek` and cleared
  only by the pacer AT AN OFFER, measured against `offeredGen`, because a seek
  can be requested, applied and settled between two pacer passes. `waitForFree`
  must return a genuinely free slot on abort (`!shutdown && !slot.inUse`):
  refusing an already-free slot instead makes the producer read to EOF
  publishing nothing, the loop is refused and the transport dies.
  (2) a seek landing while the pacer HELD a frame credited the generation
  without resetting the wrap base, so the audience saw `target + clip` —
  R19's exact symptom behind a race no driver run hit; both reconcile sites
  (loop head, and a hold broken mid-frame) now share one `reconcileSeek`.
  (3) a transport op left `status.media.playing` stale until the 500 ms
  heartbeat, so a paused background still read "playing" — the swallowed op
  wearing a different hat; `applyTransportOps()` refreshes media state on the
  same pass.
  **Fixtures:** `media_tests::exerciseSeekQueue` (deterministic protocol:
  pairing, coalescing, a stamp never handed out credits nothing, refusal
  settles without moving the applied watermark) and `exerciseMidApplySeek`,
  which parks the decode thread between performing a jump and crediting it via
  a new production seam (`holdSeekApplyForTesting`/`parkedInSeekApply`/
  `releaseSeekApply`, same precedent as `OPENSHOW_FAULT_*`) — the pairing rule
  tested ON PURPOSE rather than raced, with the older request forward and the
  newest backward so a swallowed newest runs to end-of-stream and cannot pass
  by luck; `playback_control_tests::twoTimeDomains` (deadline ≠ pts after a
  hold, and no frame waits a clip on a pts-versus-clock comparison),
  `seekAfterWrap` (backward seek after a wrap presents the target),
  `seekWhilePaused` (the scrub, plus "scrubbing is a picture, not a resume"),
  and `seekStormKeepsTheNewest` (40 overlapping pairs, jittered 0–1.5 ms
  apart, every round must reach its NEWEST target); `gpu_decode_tests::
  seekWithSlotsHeld` pins the deadlock: held leases park the producer, the
  seek still settles, a stale-frame abort is not reported as a GPU failure,
  the frame the seek made history is dropped not presented, and the target's
  frames commit on the slots the flush freed; `background_media_tests::
  exerciseSeekThroughSession` drives forward / backward-after-wrap / a 30 ms
  burst / a held scrub / a resume through a live `--socket` session and asserts
  pixels-and-position follow the media (never target+clip) with no
  `media.error`, and `exerciseSeekRefusal` pins `OPENSHOW_FAULT_AUDIO_SEEK_FAIL`
  → one refusal notification, the sticky report naming the refusal, the video
  still running, the lane honest, and the sync fault surviving the reconcile
  and the heartbeat. **Mutations** m8 (gate on pts), m9 (hold keeps `loopBase`),
  m10 (audio credits a refusal as applied), m11 (session credits a refusal),
  m12 (swallow the credit), m13 (a drop keeps its GPU slot), m14 (a purge keeps
  its GPU slot), m15 (abort left unwired), m16 (abort refuses free slots) are
  each CAUGHT; m12 survived the timing-based storm twice and is now caught by
  the seam fixture. Accepted limits: transport ops run on the render thread and
  a seek blocks up to 500 ms there (6.9b moves that wait off the loop); one
  sticky fault string, so a sync fault and an audio fault share the message.
  Gates desktop **35/35**, headless **21/21**.
- **6.8a DONE (2026-10-26).** Sizes, shapes and ranges are bounded on both
  sides of the wire (review R11). `kMaxMessageBytes` (1 MiB, payload only) is
  enforced by the SENDER — `appendFrame` returns bool now and leaves the
  caller's buffer untouched on refusal — as well as by the receiver, which
  refuses from the length prefix without buffering; the review's probe was the
  old code writing the length of a 1,048,650-byte payload and the worker
  answering by hanging up, so `WorkerController::sendRequest` builds the frame
  BEFORE the request exists: a refusal writes no byte, consumes no id, records
  no pending entry that could never be answered, and tells the operator which
  rule the content broke. `kMaxParseDepth = 8` (Boost's own default is 32)
  caps nesting — depth failure poisons the stream like garbage, because
  nesting is what a hostile payload still has once it fits the bytes — and
  `kMaxMethodNameBytes = 64` with every echoed string through `clippedForEcho`
  (160-byte budget, cut on a UTF-8 boundary: the refusal that says "too big"
  must not itself be too big, invalid, or a copy of the payload). New
  `parseRequestEnvelope`/`RequestEnvelope`/`isNotificationMethod` own the
  envelope: a stream without the version stamp still ends the connection, an
  id that cannot be answered honestly (string, bool, null, fractional, past
  `INT64_MAX`) is refused **under id 0 with the session kept** — narrowing it
  would hand the reply to some other request, which is 6.3a's correlation —
  and a no-id message is neither answered NOR executed unless it is
  `preview.released`, because a command nobody can acknowledge is 6.3a's lost
  update. The scene's budgets live with the scene: 8 layers, 256 lines, 1024
  characters a line, 16 KiB a layer's text, 64 KiB a slide's, 512-char ids,
  a 4096-char path, a 256-char font, NUL rejected in every string, a CLOSED
  key set at scene/background/item naming the clipped key (a field this build
  does not read is a field that silently never appears on air), and
  `assetPathIsLocal` refusing any URI scheme, a NUL and any other control byte
  while local paths with spaces, Unicode and `dir/sub:board.png` pass — run on
  the RAW value before the length rule, so a 200 KB `data:` URI is refused as
  a media-byte payload and not as "too long". Measured, printed and pinned:
  a representative slide is **324 bytes**, one pushed to every budget
  **408,364 bytes inside a `scene.prepare` — 38.9 % of the frame budget, depth
  5 of 8**, so nothing an operator can author is refused by the transport. The
  worker range-checks every integer where it arrives (`jsonIntInRange`/
  `jsonUint32`/`jsonUint64` replaced helpers that silently wrapped — most
  concretely `output.select` answering `static_cast<int>(4294967296)` as
  monitor **0**, a real destination the audience would have moved to), and
  `preview.start` refuses a present-but-invalid geometry instead of defaulting
  it. Locally, `sceneIsSendable` runs the WORKER's reader over a scene before
  a byte is written, and `presentSceneJson` clears the restore snapshot only
  once the new content is really on its way. **Fixtures:** `protocol_tests`
  (at-limit frame exactly 1 MiB decodes; one byte more is refused by the
  sender, buffer untouched and peer unpoisoned; depth 8 parses, 9 poisons
  saying "deep"; the echo walk; the envelope matrix), `scene_tests` (every
  budget on both sides, NUL in a line/id/path, unknown keys at all three
  levels with a 9000-byte key answered in under 400 bytes, asset references,
  the measurement), new `worker_rpc_limits` (desktop `display`: five text
  budgets refused over a live socket each followed by a `ping` that answers,
  `data:`/`http:`/`DATA:` refused where a spaced local path is accepted,
  monitor and preview arguments refused with the audience provably unmoved,
  unanswerable ids answered under 0, a bare response refused under its own id,
  a 2000-byte method refused in a reply under 300 bytes, a no-id
  `scene.present` neither answered nor executed, `preview.released` silent,
  and an over-budget frame or over-deep payload ending only the stream with
  the worker exiting **0**), and `worker_controller_tests` S8 (refused locally
  with the rule named, nothing stored, the next go still reaching air, a
  refused present keeping the restore a clear was holding, and the scripted
  peer's new `unknown` counter proving the over-budget frame never arrived
  while an ordinary call did — and because a validating client refuses a
  sketch of a scene, those fixtures now hand the controller real compiled
  scenes). **Mutations** m1 sender budget, m2 parse depth, m3 receiver
  ceiling, m8 UTF-8 walk, m9 wide id, m10 empty method, m11 method cap, m12
  line budget, m13 unknown keys, m14 NUL, m15 asset rule, m16 whole-slide
  budget, m17 monitor narrowing, m18 no-id commands executed, m19+m22 both UI
  scene-gate call sites, m20 UI frame guard, m21 restore-cleared-first: 19/19
  CAUGHT. Accepted limits: the budget is a compile-time constant rather than
  negotiated in hello; a dropped notification is logged but not counted in
  status; the notification rule is the worker's, so a third-party frontend
  that answers notifications is out of reach. Gates desktop **36/36**,
  headless **21/21**.
- **6.8b — RPC transport made stall-proof (R11) — DONE.** Nothing waits on the
  frontend at either end any more. **Worker:** the accepted socket is
  `O_NONBLOCK` and everything owed to it goes through a new header-only
  `OutboundQueue` (`src/worker/rpc/OutboundQueue.h`) whose byte budget **is**
  the transport's frame budget (`kMaxQueuedBytes == protocol::kMaxMessageBytes`
  = 1 MiB) with a 1024-frame backstop; a REPLY (empty key) is never dropped —
  room is made by evicting queued NOTIFICATIONS only, never one with
  `offset != 0` whose bytes are already in the peer's stream — and a reply that
  still will not fit sets `replyRefused`, which the caller answers by naming
  the budget in the log and closing. `serve()` polls on a 50 ms tick and
  drains on **every** tick, not only on `POLLOUT`: a trickle reader against a
  small receive buffer keeps `POLLOUT` off for whole seconds while reading
  all the same, and the gated loop stopped writing entirely — T5 caught that
  defect live. The write deadline (2 s) is measured from the **last byte the
  peer took**, so a frontend owing more than the sockets hold is dropped only
  when it stops, not when it is slow; notifications are collected into the
  queue every tick without the session lock ever being held near the socket.
  **`fConnectionFd` ownership** is fixed structurally: the serving thread
  alone closes the descriptor, under `fConnectionMutex`, clearing the member
  before the `close()`; `stop()` only `shutdown()`s it under the same mutex.
  **UI:** `kMaxInFlightRequests = 64` (a 65th is refused with the pending ids
  named), a 4 MiB unwritten-request bound whose refusal **arms** a 250 ms
  backlog watch judging idle time since `QLocalSocket::bytesWritten(>0)` and
  closing only with bytes still queued after 2 s, and one `output.status` on
  the wire at a time. **Measured:** T1 a peer reading nothing is dropped
  **2019 ms** after its last byte (898×525 B owed, no budget complaint); T2
  four frame-budgets asked, **1432 of 7999** accepted, **87,907 B** received,
  stream ended **7 ms** with the log naming the budget; T3 a deaf-but-connected
  peer served **200 commands, 0 unanswered** while 28 preview frames owed
  arrived as 4; T4 24×4 requests over 3.5 s of trickle, 0 unanswered; T5
  **400 replies (~205 KiB) owed, 60 KiB taken over 3012 ms of trickle past the
  deadline, 0 unanswered, still connected**; S9/S10/S11 pin the UI bounds, the
  in-window close ("has not read a byte") and that a peer resuming inside the
  window keeps its session. **Race evidence:** a ThreadSanitizer build (the
  missing `libtsan` rpm unpacked locally, gcc's linker script shadowed with
  `-B`) churning connect/request/SIGTERM generations shows **0** race
  signatures naming `RpcServer` on the shipped tree and **20/40** generations
  reporting exactly `RpcServer::stop()` vs `RpcServer::run()` under m16 — the
  race R11 named, appearing precisely when the mutex is removed. The ~2450
  glib/pipewire allocator races per run are pre-existing third-party noise,
  identical in both sweeps, reported as the delta rather than suppressed.
  **Mutations** m1 blocking send, m2 no deadline, m3 deadline ignoring
  progress, m15 drain gated on `POLLOUT`, m4/m5 absolute bounds, m6/m6b/m7/m8
  queue rules, m9–m14 UI rules: **16/16 CAUGHT** (m4b dropped: the frame cap
  binds before a raised byte budget at 525-byte replies, so T2 cannot see it;
  the unit fixture pins the byte bound in written-out units). Fixture lessons
  that run: three survivors were the FIXTURE's fault — bounds measured against
  the constant the mutant was raising, and mutants aimed at a fixture where
  the rule is unobservable; all fixed, with the reasons recorded in the files.
  Gates desktop **38/38** (157 s), headless **22/22** (40 s).
- **6.10 — byte-limited decoded-asset cache (R12) — DONE.** Nothing in the
  worker grows without a bound any more, and nothing the operator prepared
  can be forgotten. **Budget:** `SceneImageCache` accounts every entry by
  decoded bytes against `kDecodedImageBudget` = **192 MiB**, evicting
  unpinned entries least-recently-seen first and counting what it drops;
  `output.status.scene.images.{decodedBytes,entries,pins,evictions}` puts
  the whole picture on the wire (a new sub-object, so `hello` needed no
  capability change). **Limits before any allocation:** the file's `stat`
  against 64 MiB — a 2 GB file is refused without being read — then the
  codec's *declared* dimensions against 8192 a side and 40 MP; a
  20000×20000 header is refused on its numbers instead of by an allocation
  that might succeed and leave the machine 1.6 GB poorer. **Pins:** the
  session re-derives the whole set — {every scene in the 16-entry store} ∪
  desired ∪ applied ∪ the render thread's on-air copy, one reference per
  DISTINCT path — at prepare, at every present that changes desired, and at
  the frame commit; `syncPinSet` is release-all-then-pin-the-set, so no
  sequence of repeated prepares can drift a count, and a path the set stops
  naming is exactly a reference that ended. **The ordering law:** the budget
  resettles only *after* the survivors are re-pinned. Evicting between the
  release and the re-pin (the first version) stripped six prepared 4K scenes
  down to zero-byte placeholders on the seventh prepare — a colour fill the
  audience would have seen on a worker that had answered every command
  successfully. m11 is that mutant, and the unit scenario that catches it
  holds 90 pins = 202.5 MiB **above** the budget and re-derives the same
  set: nothing may leave, and the unpinned surplus is what pays. **Loads
  outside draw:** `imageFor()` is a pure lookup (miss ⇒ colour fill, never
  a file open — proven both ways in the fixture: a missing file's arrival
  changes nothing for a lookup, and a *cached* picture survives its file
  being deleted); decoding belongs to `loadFor`/`refreshFor` on the RPC
  thread at prepare, and `renderSceneSnapshot` preloads its own backgrounds
  so the dev CLI and the render fixtures keep working. A failure is a
  **zero-byte placeholder** the draw path never retries at frame rate while
  `refreshFor` always retries — the file the operator puts back comes back,
  and eviction forgetting the placeholder is another way the same promise
  holds. Pinned bytes may exceed the budget, because that excess is content
  that must never be paid for with a colour fill mid-show; it is reported.
  **One more visible degradation:** a still whose picture was there at
  prepare and is not at apply now *reports* (`applyBackgroundTransport`
  names it once per identity change, deferring to the preload's richer
  codec message when that is what failed). Before this change the audience
  would have got a silent colour fill and a clean status.
  **Measured:** live worker, 40 distinct 4K stills through `scene.prepare` —
  **1266 MiB asked → 506 MiB decoded, 16 entries, 16 pins, 26 evictions**,
  bounded by the store's 16 revisions rather than by the 1.24 GiB asked; the
  OLDEST still-held scene, one of the ten that recency alone could not have
  saved, then presents with no `media.error` and a clean `status.media.error`;
  and a same-path repair recovers after its failure placeholder has been
  through 12 more churns. Unit: first-load accounting (256×256 = 256 KiB, one
  4K still = **33,177,600 bytes**, the review's own number), 100 × 2.25 MiB
  flooding to 225 MiB and plateauing at 192 MiB with `evictions > 0`, pinned
  images surviving 200 distinct churns while the unpinned peer from the same
  era falls, the 90-pin over-budget sync above, a 9000×9000 header refused
  naming the dimension limit (with the refused file measured tiny), a
  7000×6000 42 MP still refused naming the megapixel ceiling (a case that
  exists because the dimension gate cannot see it), and a sparse 64 MiB+1 KiB
  file refused by its stat. **Mutations** m01 raised budget constant, m02
  eviction off, m03 eviction ignores pins, m04 `imageFor` decodes, m05
  dimension gate off, m06 file gate off, m07 megapixel gate off, m08 failure
  never retried, m09 sync never releases, m10 sync never pins, m11 evict
  mid-release, m12 session never pins, m13 broken-still report disabled, m14
  store left unpinned: **14/14 CAUGHT** (m01–m11 unit, m11 also live,
  m12–m14 live). Two escapes on the first pass were the FIXTURE's, both
  recorded in the files: the m11 geometry sat under the budget where an
  evict-mid-release has nothing to take, and m14 escaped because a
  "no media.error" check only proves something when the code can produce
  one — which is how the silent post-prepare cache loss was found. Stills are
  JPEG-generated with ffmpeg (`exit 77` when absent): this libskia decodes
  JPEG but has no PNG codec and no WPG encoder header. Gates desktop
  **39/39**, headless **23/23**.
- **6.11a — one immutable frame for output and preview (R13) — DONE.** The
  worker composes a moment once and shows it twice. `SceneFrame`
  (`src/worker/render/Layer.h`, next to `ActiveScene`, which moved out of
  `SceneRenderer.h` for the purpose) carries `{scene, fadeFrom, mediaBlend,
  textBlend, timeSeconds, wallClock, revision}`; `FrameContext` hands the
  layers that frame instead of a timestamp and the scene-aware layers keep no
  pointers to renderer state at all, which is what makes drawing into a target
  the read-only operation R13 asked for. Three calls own the contract:
  `captureFrame(clock, revision)` is the only thing that advances a crossfade
  (and stamps its origin, so the fade's 800 ms are the audience's 800 ms),
  `draw(frame, canvas, …)` composes without touching anything, and
  `commitFrame(frame)` is the only place a completed fade retires — and only
  for the frame whose scene is still the one on air, so a credited-but-lost
  frame leaves the loop owing the panel instead of stranding it mid blend. The
  mutating `draw(canvas, …, timeSeconds, …)` overloads are deleted: a target
  can no longer ask for "the composition as of now". In the session
  `fComposedFrame` is captured once per audience frame before the output draw,
  credited as soon as the present succeeds, and is what the preview draws; the
  preview-only republish draws `fPresentedFrame`, and the revision tag travels
  inside the frame, so published pixels and the published revision cannot come
  from different moments. **The audience's timing budget:** the preview is
  composed on the render thread, so it is lent 25 % of the 1/60 s frame
  (≈4.17 ms); a publish costing more stands its OWN next attempt down for
  `cost × (period/budget − 1)` so the average returns inside the share, and
  `output.status.preview.{deferred,worstMs}` put the trade on the wire
  (additive, no capability change) with `OPENSHOW_FAULT_PREVIEW_MS` placing a
  test cost inside the measured region. **A real defect fell out of the
  fixture:** the stand-down was enforced only inside `publishPreviewFrame`
  while the run loop still computed its wake-up from `fLastPreview + 1/fps`, so
  for the whole defer window the loop believed a frame it had just refused was
  still due and asked again — **58,035 stand-downs in the same 700 ms window
  where the fixed loop counts 44**. `nextAllowed` is now
  `max(cadence, fPreviewDeferredUntil)`; refusing work without sleeping
  through it is the 1.5e busy loop with better manners. **Measured** (live
  worker, 640×360 @ 60 fps, 40 ms armed cost, consumer releasing every slot):
  **published 5, deferred 44, worstMs 43.5 ms, narrowest gap between
  deliveries 150 ms, across 49 audience frames**, worst preview **lead** over
  the audience's own clock **+0.005** against a 0.2 ceiling; the same window
  with the fault off **published 29, deferred 0, worstMs 3.14 ms, gap 0** — a
  normal preview costs under the share, so nothing defers today and the policy
  only bites when the target really is expensive (a 5 ms fault, cost ~8 ms,
  defers proportionally: 23 published, 10 stood down). The CPU fixture
  (`scene_frame_tests`, both presets) shows the two targets of one frame
  agreeing to **0.11** in redness (ceiling 3.0; one audience frame's slip of
  the media track moves that metric ~7) while the fade's ends read
  **+175.08/−175.20** and its midpoint **0.00**, mid-fade ink **2031 px**
  against **1218 px** settled (both words on screen as the text track
  dissolves at 0.8/0.2) and near-white ink **0 px** mid fade against **829
  panel / 147 preview** settled — the incoming word drawn AT `textBlend`.
  **Mutations** m01 background ignores `mediaBlend`, m02/m03 incoming and
  outgoing words ignore `textBlend`, m04 `draw()` reads live renderer state
  (R13's own bug), m05 capture never stamps the origin, m06 capture ignores the
  revision, m07 commit never retires, m08 commit ignores scene identity, m09
  retire on computation, m10 budget gate off, m11 share = the whole frame
  period, m12 audience period = 1 s, m13 stand-down ignores cost, m14 the loop
  ignores the defer horizon, m15 worst cost unreported, m16 deferrals
  uncounted, m17 the preview captures a frame of its own: **17/17 CAUGHT**.
  Three escapes on the first pass, all three the FIXTURE's blind spots and all
  recorded in the files: mean-channel metrics cannot see white text alpha (so
  count near-white pixels), a preview composing its own moment is invisible to
  counts of the other target (so bound its blend against the audience's clock
  — the rule is one-directional: lag allowed, lead refused), and a `// MUTANT`
  comment swallows the rest of an `if` line. Gates desktop **41/41** (165 s),
  headless **24/24** (42 s).
- **6.11b — both fades without a render target (R13's second half) — DONE.**
  A fade wants a GROUP alpha — the blend applies to what the layers composited,
  not to each draw inside them — and the reflex answer is `saveLayer`, which is
  a second render target for every frame of every transition. Neither fade needs
  one: evaluate the group where the pixels are.
  - **Background** (`drawSceneBackground`): one `drawRect` at the media blend
  whose shader is `SkShaders::Blend(kSrcOver, Color(fill), Image(image, …))` —
  `picture over fill` per pixel, so the fill is counted once and the audience's
  picture is attenuated once. What that replaced leaks `t(1-t)(fill − picture)`
  of the incoming fill through the outgoing picture: measured **46/255** for
  opaque stills at blend 0.5, **22/255** with a translucent picture, and
  **46/255 for a scene fading to itself**, i.e. a lyric change over one still
  visibly dipped. Now **0/255** over the whole canvas on every case (opaque
  0.5 and 0.25, self-fade, translucent-over-fill, cover-cropped banded
  geometry) against an expectation written without reference to the code under
  test: each steady frame IS the group at full strength, so a fade stage owes
  the per-pixel lerp of the two steady frames.
  - **Words**: `GroupAlphaPainter`, a `skia::textlayout::ParagraphPainter`
  adapter through the public `Paragraph::paint(painter, x, y)`, scales the fade
  alpha on every draw the paragraph makes. Against the group layer it replaces:
  **1/255** worst single line, **5/255** three-line stanza (bound 12 — a layer
  attenuates overlapping antialiased coverage in full, paint-time alpha by the
  first scaled by the fade), ink bounds identical within 2 px, mid stage
  strictly dimmer. The fixture builds the FORBIDDEN layer itself as the
  reference: a test may use what production is barred from, which is how the
  replacement is proven equivalent rather than plausible.
  - **Live** (`live_fade_tests`): solid stills over a fill loud in the one
  channel both stills share, so green is a direct leak readout and the endpoints
  agree — the mid-fade answer is known in advance whatever curve the GPU uses.
  **12 frames across the fade, 9 mid-way, worst green lift −0.5** (46 against
  46) where the old composite measured **+30.8**. Still PNGs come from ffmpeg;
  absent, the scenario says so and skips.
  - **The rule** is now enforced by `render_layer_guard`
  (`tests/CheckNoRenderLayer.cmake`): no `saveLayer` anywhere in `src/` (empty
  exemption list) and no render target inside `src/worker/render` except the
  off-air snapshot entry point and the dev harness. It strips inline comments
  before matching, because m12/m13 showed a call could otherwise stand behind
  one; two negative fixtures, each tripping exactly one scan.
  - **Low-end timing** (CPU raster 1920×1080, µs/frame): words **889 steady /
  905 painted alpha / 1801 group layer** — the layer costs twice a settled frame
  for the whole transition; background **34,735 steady / 36,737 one shader /
  43,076 two draws** — correct is also cheaper. **Mutations** m01 the shipped
  two-draw fade (caught by pixels AND live output), m02 alpha twice, m03 fill
  dropped (the translucent fixture's job alone), m04 unit-square mapping, m05
  fill over picture, m06 blend ignored, m07 group at full strength, m08 painter
  ignores alpha, m09 painter alpha squared, m10 a `saveLayer` back in the words
  fade — pixel-equivalent, caught by the guard ALONE, which is what the guard is
  for — m11 a layer with its own target, m12/m13 the same hidden behind a
  comment: **13/13 CAUGHT**. Gates desktop **44/44** (175 s), headless **27/27**
  (48 s).
- **6.12 — the save adopts, and the deleted scratch stops biting (R7) — DONE.**
  Saving already copied asset bytes into the package; what it never did was
  tell anyone that the copy was now the file. Every holder of the old path —
  the live model, both history stacks, the compiled scene payloads the UI had
  already handed to the worker side — kept naming a file the save (or the
  operator) was free to delete, and the next present, autosave tick or save
  read a ghost. Now:
  - **Domain**: `saveServiceAndAdopt()` saves, then retargets every
  `Asset::sourcePath` at the package's own copy and reports the moves as
  `AssetRebind{id, from, to}`. Nothing is rebound unless the whole save
  succeeded (the model cannot point at bytes that were never copied), and an
  asset already inside the package produces no rebind at all.
  `Documents::adoptAssets` matches id AND path, so an asset re-imported after
  the save keeps its newer file.
  - **Controller**: `save()` adopts into the model and BOTH undo/redo stacks
  before `fAutosave->clear()` removes the scratch tree, and emits
  `assetsAdopted(old→new)` — a tested contract, the fixture spies on it and
  counts it: exactly one emission for a moving save, none for a save that
  moved nothing.
  - **Worker side**: `adoptAssetPaths()` rewrites `background.assetPath` in
  every payload the controller holds — revision mirror, in-flight prepares,
  the retained restore snapshot, the pending logical presentation — and
  re-prepares the scene owed to the output. Prepare, never present:
  presenting would restart a fade the audience is watching, and the worker
  needs no change because its pins are path-keyed — the on-air old file stays
  decoded until the next go names the new one.
  - **Fixtures**: storage-level (recover→adopt→scratch deleted→save again→
  tick again, failed-save adopts nothing, autosave ticks do not adopt, the
  match rules); the controller's real-file R7 sequence (import→crash→recover
  →edit→undo→save→redo→delete the original→present/edit/undo/redo→second save
  succeeds — pre-task that save failed "has no readable file", and the tick
  after the first save had already failed silently against the deleted
  scratch); and S12's scripted-peer ledger — seven prepares, the old path
  named exactly once and never by anything the adoption ran over, restore
  after a snapshot-time adoption naming the new file, adoptions moving the
  audience not at all, a map naming nothing sending not even a byte.
  - **Mutations** m01 never-adopts, m02 adopt-before-success, m03 always-
  rebinds, m04 ignores the `from` match, m05/m06 undo/redo stack untold,
  m07 the old const save in `save()` (R7 itself), m08 signal never emitted,
  m09 no re-prepare, m10 presents instead of prepares, m11 adoption no-op,
  m12/m13 restore snapshot / mirror keep the old name: **13/13 CAUGHT** —
  m12 escaped the first pass and the scenario, not the code, was wrong: it
  adopted while the restore slot was empty, so the snapshot inherited an
  already-rewritten payload (see Decisions). Gates desktop **44/44** (180 s),
  headless **27/27** (48 s).
- **6.13a — saving becomes a commit: staged assets, synced manifest,
  reported refusals (R8) — DONE.** The manifest already committed through
  tmp+rename; the asset copies did not, and `copy_file(overwrite_existing)`
  could smear half-written bytes over the very file the still-valid
  manifest named (the review's measured probe). Saving is now a commit
  protocol:
  - **Assets** stream into a unique `.…openshow-staging-<pid>-<n>` file
  inside the package's `assets/`, are fsynced, then renamed onto the
  destination. Rename is atomic — a reader of the previous manifest sees
  either the whole old file or the whole new one — and an interrupted copy
  dies under a name nothing references. A changed asset gets its new
  version under the same name (documented: never torn, possibly newer than
  what the old manifest was saved against).
  - **Manifest**: raw writes (write() returning IS the flush — nothing is
  left in a user-space buffer), fsync, close with its status **reported**
  (the old code checked good() and let the destructor swallow the close
  failure — R8's exact half), rename, then the containing directory. The
  assets directory is synced BEFORE the manifest is written: the manifest
  commits only after all referenced bytes are durable.
  - **Durability defined** (storage.h): a save that returns true has
  synced every file and every naming directory entry to the device — the
  commit survives subsequent process crashes and power loss, as far as an
  honest fsync can promise. The one reported failure that can leave the
  new manifest installed (the final directory sync) says exactly that,
  and the package is consistent either way. `Autosave::tick` rides the
  same commit.
  - **Fault seam**: `OPENSHOW_STORAGE_FAULT` refuses one named step per
  save, read at each step so a fixture faults, recovers and re-commits in
  one process. `exerciseCommitAtomicity` runs all nine steps against an
  existing package: refusal reported naming the step, previous manifest
  byte-identical, packaged asset never a torn mix (the asset-copy round
  checks it byte-for-byte — R8's detector), package loadable with assets
  round-tripping, zero staging or tmp debris; then the clean retry lands
  model and asset whole, re-save is byte-identical (staging names never
  leak into the manifest), and a refused autosave tick names its step and
  leaves no manifest.
  - **Mutations** m01 copy-overwrite restored, m02–m08 each barrier or
  refusal made deaf (m06 is R8's close-swallow), m09 manifest written
  straight over service.json, m10/m11 debris left behind, m12 manifest
  committed before the assets-directory barrier: **12/12 CAUGHT** with
  blame on precisely the intended checks. In-process tests cannot see a
  silently-DELETED fsync, so barrier presence on the success path is
  evidenced outside the test: strace counts **93 fsync calls** on a clean
  domain_tests run. Gates desktop **44/44** (180 s), headless **27/27**
  (48 s).
- **6.13b — loaded ids are names, and a package cannot name the world
  (R8's second half) — DONE.** A service package can arrive from an
  archive, a stick or a shared folder, and the reader trusted its
  strings: loaded ids were merely nonempty/unique while an asset's id
  prefixes the filename its bytes are written under, and the recorded
  asset `file` accepted absolute paths and `..`. Now:
  - **Ids**: `safeOpaqueId` gates `claimId` — the single chokepoint all
  six id spaces pass through — to letters, digits, `-` and `_`. The
  store's own `asset-12` form always passes; `../../evil`, `sneaky name`
  and `x.png` are refused naming the id. Because references must resolve
  to claimed ids, one rule covers the whole id space.
  - **Containment**: an absolute `file` or any `..` segment is refused
  by name before any filesystem call; then `canonical` has the final
  word — every symlink inside the package is followed to where it really
  leads, and the real path must sit inside the canonical package
  directory compared COMPONENT-wise. The fixture's sibling
  `file-4-evil` directory (whose name starts with the package's, reached
  through `assets/sib.jpg`) is exactly what catches the
  string-prefix-is-not-a-boundary bug (m05). Order stays specific:
  name refusals, the existing missing-file message, resolution, then
  containment.
  - **The friendly side of the rule is pinned too**: an inside symlink
  loads happily (containment, not link paranoia), a charset-legal
  renamed id loads with its bytes, an unruly asset NAME still lands as
  one safe component inside the package, and authorised import still
  copies external files at save — the operator's choice, not the
  manifest's. Every refusal leaves the target Documents untouched and
  touches nothing outside its package.
  - **Mutations** m01 charset dropped, m01b charset applied to assets
  only (the show-id vector bites), m02 absolute accepted, m03 climb
  accepted, m04 name-only containment (symlinks trusted), m05
  string-prefix containment, m06 containment dropped: **7/7 CAUGHT**
  with blame on the intended check (on top of 6.13a's 12/12). Gates
  desktop **44/44** (180 s), headless **27/27** (48 s).
- **6.14a — the crash net reports its own holes, and quitting commits
  the last keystrokes (R14's first half) — DONE.** Five sites in
  `DocumentController` were discarding persistence errors; all five speak
  now:
  - **Refused autosave ticks** (edit/undo/redo) go through
    `surfaceAutosaveResult`: the status reads "autosave refused after
    edit|undo|redo: <disk error> - unsaved work is NOT on disk". It is
    DEFERRED on a zero timer because edit callers write their own status
    synchronously after `edit()` returns and last-write-wins — the
    fixture proves the deferral is load-bearing through `importMediaFile`
    ("added to the media library" lands first; the refusal overwrites it
    as the last word). A `fAutosaveStale` flag guards the race — a
    refusal queued in the same turn must not stamp a session whose
    scratch has already caught up — and the catch-up itself says
    "autosave recovered - the scratch on disk is current again".
  - **save()/load() whose `clear()` fails** still succeed (the service
    commit worked), but name the survivor: "the stale autosave could NOT
    be removed... a later start may recover work this save already
    replaced". R14's silent resurrection is now a visible warning.
  - **The quit flush lives in the controller's own constructor**
    (`connect(qApp, &QCoreApplication::aboutToQuit, this,
    &DocumentController::flushText)`): a window close and the
    SIGTERM/SIGINT poll both end the loop through `aboutToQuit`, so
    close-inside-the-debounce commits the pending text and the dirty
    tick leaves the final words in the scratch for the next start. The
    earlier design (wire it in `main.cpp` + `Main.qml onClosing`) was
    replaced during implementation: the invariant belongs with the state
    it protects — it cannot be forgotten by whoever wires `main()`, needs
    no QML change, and becomes testable by firing the real `aboutToQuit`.
  - **Fixtures** (`exerciseAutosaveSurfacing`): quit inside the 600 ms
    debounce via a real `QCoreApplication::exec()` exited by `quit()` —
    the next session recovers the final keystroke, with a no-quit
    control proving the text IS otherwise lost; `chmod 0555` on the
    scratch → refused edit, undo and redo each named on the status line
    after `processEvents` (undo/redo retry because `tick` does not poison
    its fingerprint on failure), plus the last-word import case, the
    healthy-again line and the race-guard sequence; `chmod 0555` → save
    and load both name the undeletable stale autosave.
  - **Mutations** m01–m03 each tick site deaf again, m04 refusal branch
    dead, m05 synchronous status, m06 race guard dropped, m07
    healthy-again silent, m08/m09 the save/load clear swallows (R14's own
    silences), m10 the aboutToQuit connect removed, m11 flush gutted:
    **11/11 CAUGHT**, each on exactly its intended check. Gates desktop
    **44/44** (181 s), headless **27/27** (48 s).
- **6.14b — persistence moves off the UI thread on a bounded, coalescing
  executor; an older write can no longer overwrite a newer decision
  (R14's second half) — DONE.**
  - **Domain (`AssetReuse` ledger in `Autosave`)**: a scratch tick may skip
    re-copying an asset only when BOTH sides still identify — the source is
    the same file (canonical path + size + mtime) whose bytes were last
    installed, AND the destination file still exists. Stat identity is the
    documented edge; any stat failure means re-copy. `recover`/`clear`
    reset the ledger; `ServiceCodec::save` takes the ledger as a defaulted
    argument that ONLY `Autosave::tick` passes — `saveService`/
    `saveServiceAndAdopt` always copy a whole package. Text edit after a
    video import: zero byte-copies.
  - **UI (`AutosaveScheduler`)**: one job running (scratch writes
    serialise), one replaceable pending slot — coalescing keeps the NEWEST
    immutable `Documents` snapshot copied on the UI thread, so the freeze
    R14 measured becomes a memcpy. Decision protocol: `save()`/`load()`
    call `quiesceForDecision()` (raise epoch, drop pending, WAIT out the
    in-flight write) before touching the scratch, `resumeAutosave()` after
    — every early-return path resumes; the epoch additionally vetoes stale
    requests, running jobs and their acknowledgements. Acks
    (`tickCompleted`) queue to the UI thread and are emitted BEFORE the
    running-flag clears, so `awaitIdle()` returning implies the ack is
    already in the UI queue. Dormant by default — inline ticks, the exact
    pre-6.14b behavior — so no existing fixture changed; `main.cpp` opts
    in with `startBackgroundPersistence()`. `aboutToQuit` now runs
    `prepareForQuit` = flushText + drain: the quit promise is durable, not
    queued.
  - **Seam**: `OPENSHOW_STORAGE_DELAY_MS` (read per `save()`, capped at
    3000 ms, sleeps before ANY effect) lets a fixture hold one write in
    flight while a decision runs fast — the only way to schedule the race
    R14 named, and m05 proves the fixtures actually witness it (save
    measured waiting ≥400 ms of the armed 600 ms).
  - **Fixtures**: domain `exerciseAssetReuse` — text-only tick survives the
    `asset-copy` refusal (skip visible), changed bytes / same-size-new-mtime
    replacement / deleted destination each force the re-copy, `clear`
    forgets the ledger, a recovered asset needs no self-copy. Controller
    `exerciseBackgroundPersistence` — (a) 30 commits vs a 250 ms disk: UI
    loop <120 ms, ≤5 writes, ≥20 coalesced, scratch holds "storm line 29";
    (b) save vs in-flight 600 ms write + queued request: waited ≥400 ms,
    clear is last word, sleeper never lands; (c) same on the load-discard
    path; (d) background refusal + healthy acks surface on the status line;
    (e) `prepareForQuit` drain — next session opens on the last words.
  - **Mutations** m01–m04 the four reuse-rule legs, m05 the dead seam,
    m06/m07 the two quiesce removals (R14's resurrection restored), m08 no
    quit drain, m09 always-submits, m10 keeps-oldest-pending, m12 pause
    without the wait: **11/11 CAUGHT**. Two battery lessons paid for in
    escaped mutants: a trailing `//` inside a patched expression swallows
    the rest of the line (use `/* */`), and fixture (3) was one byte long
    in its "same size" replacement — an mtime-blind rule still saw a size
    change and passed vacuously; equal length is now load-bearing in the
    comment. The pause-drop of pending is behaviorally equivalent to the
    epoch veto (defense in depth), so it has no separate mutant.
  - Gates desktop **44/44** (185 s), headless **27/27** (48 s);
    `document_controller_tests` green ×3 consecutively (threaded fixtures).
- **6.15a — the transport answers the operator over the socket; UI→RPC→
  transport fixtures prove it (R15) — DONE.**
  - **Protocol (minor 5→6)**: capability `media.transport.v1`, constants
    `kMethodMediaPlay/Pause/Seek/Loop/Gain/Mute` and the source identities
    `"video"`/`"standalone"` — the identities on the wire now means 6.15b's
    standalone track needs no further wire change. Scene format untouched:
    a scene still only REFERENCES media; every transport command rides the
    RPC.
  - **Worker**: `OutputSession` gained `requestMediaLoop/Gain/Mute` beside
    the existing seek/pause — pending slots the render thread applies next
    frame beside the transport (newest-wins, cleared at teardown before
    `live` drops), plus `mediaTransportLive()`/`mediaSourceLive()` mirrors
    and a source-registration mask set at bring-up. A gain/mute naming a
    source this transport never registered is a REFUSAL naming it — the
    AvPlayer's unregistered-identity passthrough stays below the wire;
    nothing evaporates over the socket (R19's lesson one layer up). The
    RPC handlers validate before believing (`jsonDoubleInRange`: every
    Boost number kind accepted, NaN and out-of-range refused, seek ceiling
    86400 s = a bound on nonsense, not a promise about durations), refuse
    commands with no transport per command leg, and ack `{"pending":true}`
    — status is the acknowledgement (3.4's rule). Status `media` gained
    `live/loop/duration/sources[{name,gain,muted}]`.
  - **UI**: five `media*` properties + six invokables + a transport row
    (hold/resume, loop toggle, scrub bound to the ACKNOWLEDGED position,
    per-source gain slider + mute). Controls mirror worker status, never
    the click; sliders reapply status values only while
    `!pressed && !dragging`; `live` — never `path` — is the enablement
    signal, because 5.5d leaves path with two teardown flavors (a failed
    asset keeps it so the sticky error stays paired with its file; a
    background-less apply clears it) and a stale path would make a dead
    transport look commandable.
  - **Fixtures** (`background_media_tests`, live over real `--socket`):
    pre-bring-up refusals witnessed per gate (play/pause, seek, loop,
    gain — each command refused NOW with its own message check, not
    hiding behind a sibling); live/loop-default/duration 6.00 and NO
    source listed for the silent `-an` clip; pause parks the position
    exactly (measured 0.04–0.08, stable); forward (5.6) and backward (1.0)
    seeks acknowledged through status; loop-off ends once — playing false,
    error empty, position settled 5.96 and holding (the check POLLS for a
    settled position because decoded frames drain ~2 past the EOF flag:
    5.88 → 5.96 → stable; a fixed-sleep exact match escaped once and was
    rewritten as the honest promise), zero "ended while looping" pushes;
    teardown takes `live` with it, no source survives, commands are
    refused again; gain 0.4 + mute/unmute acknowledged via the mixer echo;
    the absent standalone source refused BY NAME on both the gain and the
    mute leg; a refused seek over RPC = sticky status error + pushed
    `media.error` naming "refused" while the picture plays on.
    `worker_controller_tests` S13: a scripted peer whose transport state
    ONLY the `media.*` commands move, echoed through `output.status.media`
    — the controller's properties move exactly with that echo (a refused
    standalone gain leaves the mirror on the worker's truth, the client
    never invents a row), and a verbatim `mediaOps` ledger pins the exact
    method-and-params frames that rode the wire, including the refused
    standalone gain the client sends rather than second-guess the worker's
    source table.
  - **Mutations** m01–m03 the three refusal gates dead, m04 seek range
    opened to negatives, m05 non-numbers coerced to 0, m06 unknown source
    accepted, m07–m09 the session's live gate and both source gates,
    m10–m12 the three applied slots dead, m13 the source mask never
    registered, m14 `live` never seeded, m15 teardown leaves `live` true,
    m16 the status sources array emptied, m17 the ack ternary promoting
    bool to double, m18 the controller deaf to status media, m19 the
    capability unadvertised: **19/19 CAUGHT**, each on its intended check
    (m17's is the test's own `as_bool()` throw — disclosed). Disclosed
    as not separately mutable: `jsonDoubleInRange`'s NaN leg (no JSON wire
    value carries NaN to it — the serializer sends null), and the
    teardown's mask/pending clears (individually equivalent behind m15's
    live leg).
  - Battery lessons: suites must be LISTS OF argv LISTS — a flat argv list
    ran the test with no worker ("never answered") and the bare worker
    (usage, rc 2) and misread every mutant as caught on the wrong evidence
    (run 1 discarded); the `//`-in-patched-expression trap struck again
    (m04 BUILD-FAILED, `/* */` since); and a dry-run patch script must
    never `open(path, "w")` as an expression argument before its data
    lookup — that zeroed `RpcServer.cpp`, restored from HEAD + the three
    edit groups reapplied verbatim, faithfulness proven by full rebuilds
    and both live suites green.
  - Gates desktop **44/44** (192 s), headless **27/27** (48 s); offscreen
    QML smoke zero errors.
- **6.15b — a production audio track with a lifetime of its own (R15) — DONE.**
  - **Protocol (minor 6→7)**: capability `media.standalone.v1` and the
    `media.standalone {"path"}` command (`""` is the removal), answered by
    the track's OWN status block `media.standalone {live, path, playing,
    position, duration, error, audio, loops}`. Two transports, two reports:
    the background's fields keep describing the on-air video transport and
    these keep describing the track, so 5.5d's "a sticky error names the
    file it describes" holds inside each block without either one
    contaminating the other.
  - **Where the track lives is the design decision.** It is a second
    `AvPlayer` (`fStandalonePlayer`), NOT a second mixer input on the
    background's: the background transport is torn down and rebuilt at every
    background-identity change, so a track riding it would restart at exactly
    the moment the acceptance forbids. PipeWire mixes the two sink streams
    server-side, disclosed as the honest shape — the session has no business
    folding two lifetimes into one graph just because one component could.
    Selection PROBES synchronously on the RPC thread (same probe
    `scene.prepare` runs, file I/O under no lock), so "nothing readable at
    that path" is a refused request the operator hears now with the old track
    still playing; an accepted path rides a pending swap applied first in
    `applyTransportOps`. Loop is always on — the removal is the stop.
  - **`loops` exists because the clock cannot say it.** The position an
    audio-only track paces by is the HEARD sink clock, which runs past the
    file's end whether or not the feeder re-reads it (a starved mixer pads
    silence and the server consumes that at the same rate), so a bed that ran
    out and a bed that rewound looked identical. The block now publishes the
    feeder's wrap count: N wraps means the audience is hearing pass N+1, not
    silence. Writing this fixture is what found the misunderstanding.
  - **Gating is per identity** (R19's discipline continued): `kVideo` gates
    on the background's live flag plus its registration bit (a playing silent
    clip is live without one), `kStandalone` on `standaloneLive()` — for the
    track, live IS registration, because the swap that starts the player
    stores bit1 and the one that stops it clears it, so there is no second
    kind of absent to refuse. Background bring-up and teardown now
    read-modify-write only their own mask bit and drop pending intents per
    identity, so the background can neither unregister the track nor queue a
    command for it; `close()` ends the track by name for the same reason it
    is excluded from the background's lifetime.
  - **Fixtures** (`background_media_tests`, 68 check sites, two worker
    sessions): audio-only comes up live with duration 6.00 as the ONLY row
    and advances at a clock's rate while background commands still refuse
    beside it; refusals witnessed per probe leg (an open failure must not read
    as a stream complaint, and the `-an` clip witnesses the stream leg naming
    its own file); mix — the embedded background goes on air beside the
    playing track, both rows listed, the track keeping the gain the operator
    set before the background existed, muting either source leaving the other
    exactly right on the worker's echo; a lyric change over the same identity
    and a full background REPLACE (transport torn down and rebuilt around a
    playing track) neither restart it nor lose its row or gain; **removing the
    track while the background plays on** — the only moment the per-identity
    gate is observable: live and the row leave, the embedded row keeps its
    distinguishing 0.6 and stays addressable, and a track gain AND a track
    mute are refused naming the track, neither refusal moving the other
    source; re-select starts at unity beside the background's 0.6; background
    removal leaves the track alone, live and still the only row; `""` takes
    live and the row with it and commands refuse rather than queue for a
    successor; the loop — wraps reach 1 with the block still live, playing and
    error-free; a mixer-fault bring-up (the probe passed, so the ack was
    "pending") lands as the block's own sticky error naming the track, `live`
    false, no row, and a pushed `media.error` naming the track's path, worker
    alive. `worker_controller_tests` S13 scripts the same track (accept, a
    "missing" path refused, `""` removal, sources built from what is actually
    up, a fresh track at unity while the background keeps its value): the
    controller's `standalone*` properties move only with that echo, and the
    verbatim ledger pins the selection, the refused path and the removal as
    frames.
  - **A bug the fixtures caught in flight**: `refreshMediaStateLocked` sampled
    the track and then returned early with no background — before the sources
    rebuild — so in an audio-only session a gain reached the MIXER and the
    status row sat at whatever the swap had written. The echo now rebuilds on
    that path too (it reads atomics from mixers that already exist), which is
    exactly what the first live run's two failures said.
  - **Mutations** m01–m21: both track gates (gain and mute separately), the
    probe believing an unreadable path, both background paths clobbering the
    track's mask bit, the unpublished mix echo, gain and mute routed to the
    background's mixer, live surviving removal, wraps never sampled, the bed
    not looping, `playing` inverted, position never sampled, the pushed fault
    naming no path, the failed bring-up saying nothing, `close()` leaking the
    player, the status block dropped, the selection acked but never handed to
    the session, a deaf controller, the wrong method on the wire, the
    capability unadvertised: **20/21 CAUGHT**. m16 is disclosed
    mutant-equivalent (`~AvPlayer()` calls `stop()` — that call is intent, not
    mechanism). Two survivors became STRONGER CHECKS instead of comments:
    m03 survived because the next probe leg refused the same file for a
    different reason and my check only looked for the path; m13 (position
    pinned to 0.0) survived because the block seeds `position` at -1, so the
    check compared a sentinel jump as motion — now the fixture waits for a
    SAMPLED position and bounds the step. Disclosed unmutables: the "no
    decoder" probe leg (no fixture codec provokes it), the swap's
    per-identity pending drop and its ordering ahead of the ops (needs a gain
    accepted for a live track AND a re-selection in one render pass, which the
    live gate makes unschedulable), the lane seeded at the swap (the refresh
    writes the same value), and the minor/method-name constants (both sides
    move together).
  - Gates desktop **44/44** (196 s), headless **27/27** (48 s); offscreen
    QML smoke zero errors.
- **6.16a — the editor's picture comes from the renderer, not from a second
  text layout (R16, first half) — DONE.** `EditCanvas.qml` painted a QML
  `Text` over a rectangle and called it a preview of the selected slide:
  Qt's wrapping, Qt's fit, Qt's font fallbacks, measured against Skia's, and
  nothing at all about the scene's media. That twin is deleted. The canvas
  now paints the worker's own render of the scene the operator has selected,
  drawn while it is NOT on air, under the QML handles.
  - **One stream carries both targets.** A second segment for the same
    consumer would have duplicated the slot accounting, the identity rule and
    the drop-don't-block producer to carry the same kind of pixels, so the
    layout is versioned 1->2 and `PreviewSlotHeader` gains `target` and
    `request`; `preview.ready` carries the same two values under the same
    seqlock, so metadata and pixels agree on one read, and a version-1 frame
    with neither field reads as the audience — all it ever could be. New
    method `preview.select {revision, request}`, protocol minor 7->8,
    capability `preview.select.v1` (11 advertised).
  - **The generation rule has two halves and both are enforced.** The
    frontend owns a monotonic counter; the worker accepts only a request
    strictly after the accepted one (modular order) and refuses naming the
    action — start the stream, re-prepare because the store is bounded,
    advance the counter. The consumer mirrors it: a frame whose request is not
    the newest it sent is released unpainted. The producer's half alone would
    not help, because the honest race is that the render thread was already
    drawing the ask the operator has moved past.
  - **"Off the air" is asserted, not assumed.** The selection frame is built
    by hand — pinned scene, `fadeFrom` null, blends at full strength, the
    scene's own revision — and `publishSelectionFrame` never calls
    `captureFrame` (the only call that advances a crossfade), `commitFrame`,
    `setActiveScene` or the transport's apply. Six asks fired during a live
    crossfade leave the audience's active scene id, its presented-frame
    counter and its own mid-blend mirror exactly as they were. Video is
    sampled only when the off-air scene names the same asset: an unpublished
    scene has no transport, so any other video draws as the colour fill it has
    no frame for, rather than wearing the audience's moving picture.
  - **Pinned at the accept**, not at the draw: the accepted scene becomes a
    `shared_ptr<const Scene>` and joins the pin set inside the lock that
    accepts it, because entries are immutable per revision, the store may
    evict before the pass that draws, and the frame loop must never open a
    file (R4's rule, applied to a target that has no present to pin it).
  - **One stream means one budget**, so the targets take turns:
    `drawPreviewTarget` publishes at most one frame per pass, whichever has
    gone longest, each with its own cadence marker and a shared deferral, and
    the run loop sleeps to the sooner horizon. The generation is re-read under
    the accept lock AFTER the CPU-synchronous readback; a frame that no longer
    answers the newest ask is `abort()`ed — the slot closes on its previous
    value, so the consumer never sees half a frame — `stale` moves, and the
    ask stays owed for the newer one.
  - **A renderer cost the two targets found**: the words layer's layout cache
    held two slots, and alternating the audience's scene with a pinned copy of
    the same scene rebuilt a layout every single pass. It is a four-slot LRU
    now, with `SceneRenderer::textLayoutBuilds()` published so the fixture
    measures rebuilds rather than trusting the size.
  - **UI**: asking is `scene.prepare` — deliberately NOT `armPrepare`, which
    would make it the next thing to present — plus `preview.select` on that
    ack. Main.qml debounces selection at 250 ms across document, text and
    navigation changes, re-asks on a `preview.start` ack (a restarted stream
    dropped the target) and on one status reporting `selection.active` false
    while an ask stands, and stands down when the page is left. `PreviewView`
    gained a `Target` enum; the canvas says "waiting for the worker's render
    of this slide" versus "this worker cannot render a slide that is not on
    air", so an old worker is a stated limitation instead of a silent blank.
  - **Fixtures**: new live `preview_selection_tests` (display label, two
    worker sessions) — the edit's pixels ARE the blue scene, slot header and
    notification agree, the audience's scene, counter and mirror unmoved, one
    frame per accepted request across a burst with strictly increasing
    generations and nothing after the last ask, an unknown revision refused
    naming the store, a replayed generation refused twice over with the
    accepted number unmoved, edits through a live crossfade, the stand-down
    stopping frames, refusal with no stream, and — on a worker with an
    expensive draw armed — the superseded draw never reaching the wire with
    `stale >= 1`. `worker_controller_tests` S14 turns the scripted peer into a
    REAL producer of the transport: its own shm segment, seqlock, a slot freed
    only on a release that proves ownership and sequence, a verbatim
    `preview.select` ledger, and two markers in the prepared scene's slideId —
    `SLOW` holds a frame 500 ms so the stale frame the consumer must drop
    actually exists, `FORGET` reports the target gone once so the re-ask is
    behaviour under test. It asserts capability gating, two panes with
    different pixels, a companion audience frame landing behind every edit
    without trading places with it, no `scene.present` ever, dedupe, the late
    frame dropped before the copy, the stale counter mirrored and ending at
    exactly one, recovery asking exactly once, the whole ledger
    (1,1) (2,2) (3,3) (4,4) (5,5) (6,6) (0,7), a version-1 frame with no
    target routed to the mirror, and zero frames dropped for want of a slot.
  - **Mutations** m01–m22 plus m04b — the stream, store and stand-down gates,
    the once-per-ask rule everywhere, the publish gate, both targets in one
    pass, the presented counter bumped by an edit and frozen, the slot
    header's target, the layout slots, ready's two fields, the method, the
    status block, the capability, and on the client dropping ready, an open
    generation gate, no dedupe, no re-ask, no stale mirror, wrong-pane
    routing, a peer counter turned unconditional, a QML property error:
    **21/23 CAUGHT**. Run 1 left five alive and three were weak checks, now
    strengthened and caught — m07 (`status.frames` is written at the end of a
    composed pass, so an idle audience never republishes a bump; the fixture
    now asserts the CONSEQUENCE: an edit draw creates **no mirror debt**), m20
    (the peer had never sent an audience frame while an ask stood, which is
    precisely the race the routing exists for), m22 (qmlcachegen makes a bad
    property fatal AND silent, so the smoke now requires the app to be alive at
    its `timeout` deadline as well as silent). Disclosed equivalents: m04 —
    the same predicate guards three layers, and m04b removing all three is
    caught; m06 — the per-pass `return` bounds draw cost, which no fixture can
    measure without a stopwatch on a shared desktop; `selection.dropped` and
    `.deferred`; the pin set's membership (needs >16 background-holding
    scenes); `PreviewView::paint()`'s target ternary; the version constants.
  - Gates desktop **45/45** (210 s, one test added), headless **27/27**
    (48 s); both trees build without warnings; offscreen UI smoke silent and
    alive at its deadline.
- **6.16b — the slide list draws from the worker, one row at a time (R16,
  second half) — DONE.** `Main.qml` listed slides as `(index) + preview` text
  lines laid out by Qt while the worker drew the audience something else, so
  the operator read one renderer's idea of a slide beside another's. The list
  now asks the worker to picture the visible band and paints what comes back,
  with the text line kept underneath as the fallback it always was — legible
  before the picture arrives, on a worker that cannot draw the list, and to a
  screen reader either way.
  - **A third target on the stream 6.16a versioned, not a third segment.**
    `preview.thumbnails {scenes,width,height,request}` answers
    `{request,width,height,frames[{key,id}],refused[{key,why}]}`; rows are
    `kPreviewTargetThumbnail` frames whose `revision` is a worker-assigned
    per-slide id from the ack and whose `request` is the batch generation;
    capability `preview.thumbnail.v1`, minor 8→9, 12 capabilities. A batch
    **replaces** the pending set wholesale — cancellation as one comparison,
    because a scroll wheel fires this command — and an empty batch is a
    stand-down that still consumes a generation. Bounds live in
    `preview_wire.h` so the worker, the client and the scripted peer judge a
    batch by one predicate.
  - **A row's size is part of the frame.** A slot's payload was reserved for the
    stream's own extent, so `PreviewStream::publish` takes an optional
    `FrameExtent` and stamps `width/height/rowBytes` on every publish,
    `bytesPerSlot()` is published, and the extent is re-checked at publish time
    as well as at accept: a smaller `preview.start` that found a batch standing
    would otherwise mean writing 921,600 bytes into a slot reserved for 230,400,
    which is not a wrong picture but the worker's memory.
  - **Isolation is the acceptance.** `SceneRenderer::createCompanion` gives the
    list its own words-layer LRU and its own 12 MiB `SceneImageCache` while
    **sharing** the `SceneTextEngine` — one font engine, two pools — and a
    thumbnail scene never enters `fScenes`, the prepared store or the pin set.
    Decodes run on the RPC thread, one file per 50 ms tick, lookahead 4 past the
    draw head; the draw path never opens a file and a miss is the colour fill.
    `SceneImageCache` gained `install()`, a per-instance budget honoured by
    `evictLocked`, and `decodeAtMost()`, and `decodeImageFile` resamples
    anything still over the box after `RasterFromBitmap` — the box is a
    promise, not a preference — so a backdrop for a 160x90 row is ~57 KB
    cached rather than 33 MiB. Nothing in the path calls `captureFrame`,
    `commitFrame`, `setActiveScene` or the transport's apply, and the live
    fixture measures the show's presented-frame counter, decoded pool and
    mirror unmoved by a band of thirty.
  - **The client mirrors the selection's pattern.** wanted/asked/acked state, an
    id→key map that lives only as long as its ack, three refusal gates before a
    byte is written (cap, box, this stream's slot), R11's per-entry rule applied
    to a batch, dedupe on payload+extent, and a cache bounded twice — 64
    entries and 4 MiB, whichever ceiling bites first. A frame is copied only if
    it answers the accepted generation, names an id an accepted batch handed
    out, and its slot header agrees with its notification about its size; an
    edited slide loses its old picture the instant it is edited; a worker that
    reports the band gone is re-asked exactly once. One row's arrival repaints
    one item, not the list.
  - **Four bugs, all found by fixtures or smoke.** `fillPreviewSlot` accepted a
    `SceneRenderer*` and an `sk_sp<SkSurface>*` and drew with
    `fScene`/`fPreviewSurface` anyway — rows came back the scene's FILL colour
    and every row alternation reallocated the mirror's surface; `PreviewView::key`
    was an accessor pair with no `Q_PROPERTY`, which compiles and takes the whole
    QML module down at load; three `root.` references where the function and flag
    live on the `showPage` Item, plus a `Qt.callLater` handed an undefined value;
    and the scripted peer derived `active` from its remaining row debt, so a
    finished list looked lost and the client re-asked one band four times a
    second.
  - **Tests.** Live slide-list phase (three rows are three slides by colour
    identity; every refusal naming the number it failed; the show's pool
    untouched while a backdrop row decodes to <=64 KiB in the list's own pool; a
    20-row band replaced by 1 owing <=1; **leg 9**: a standing 480x480 band and a
    smaller `preview.start` end stood down, undrawn, with a fresh 64x36 ask
    landing at its own extent). S15 makes the scripted peer a real producer of
    rows — monotonic ids, one row per wake, `LATE`/`OLDSTAMP`/`LIE`/`FORGET`
    markers — so each of the three ways a frame must not be painted exists on
    the wire and is shown dropped before the copy, with the 4 MiB cache measured
    while 32 rows at the stream's extent arrive. New ctest `ui_loads` runs the
    real shell offscreen and requires it to STAY UP and say nothing about loading,
    with two negatives so the guard is not vacuous.
  - **Mutations** m01–m20 plus m04a/m08a/m10a: **19/23 CAUGHT**, with blame on
    the intended checks (the shipped `fillPreviewSlot` bug failed the backdrop's
    colour identity, the un-resampled PNG failed the pool's byte measurement, the
    removed cache ceilings failed the 4 MiB ceiling and the LRU order). m19 was
    re-scored after fixing the driver: the protocol library had been left out of
    the rebuild set, so run 3 scored it on a binary that had never been rebuilt,
    and a mutant that cannot run is a driver lying rather than a fixture passing.
    Disclosed: m04a/m08a/m10a are the redundant halves of pairs the battery does
    test (the wire refuses the cap and the key before the session is reached; the
    start path stands a batch down before the draw path can be owed); m20, the
    client's id-map gate, is defence in depth — a frame whose id no accepted
    batch named is copied and then refused by the store step for want of a key, so
    nothing observable moves (what the gate bounds is per-frame copy work).
  - Gates desktop **48/48** (228 s; `ui_loads` plus its two negatives added),
    headless **27/27** (48.5 s); `preview_selection_tests` 14.3 s,
    `worker_controller_tests` 47.1 s, `scene_image_cache_tests` 1.94 s,
    `ui_loads` 8.03 s; both trees build clean apart from the pre-existing GCC
    `-Wfree-nonheap-object` false positive at
    `tests/background_media_tests.cpp:2294` (6.15b test code); offscreen UI
    loads with the five pre-existing QML notices and no load error.
- **Task 6.17a1 DONE — a slide is a stack of text boxes (review R17, first
  half of the 6.17a split).** `domain::Slide` no longer owns one lines/geometry
  tuple: it owns `items`, an ordered `std::vector<TextBox>` in DRAW order, each
  box carrying a store-issued id, its own words and its own 1920x1080
  rectangle. The old representation is gone rather than shadowed — there is no
  `slide.lines`, no `slide.x` — because a model that keeps both makes every
  reader pick a favourite and every writer remember both, and the first reader
  to disagree is the preview an operator trusts.
  - **The legacy pair still works, and says what it addresses.**
    `setSlideLines`/`setSlideGeometry` are documented as addressing box 0 in
    draw order — the only box a pre-6.17a slide has — and a slide drained to no
    boxes REFUSES them ("slide '…' has no text box to write into") instead of
    growing a box back under whoever was typing. The new operations own the
    rest: `addTextBox` (appends on top; **the caller's id is dropped**, so a
    clip or a template cannot smuggle a duplicate identity in), `removeTextBox`,
    `setTextBoxLines`, `setTextBoxGeometry`, `moveTextBox` (destinations name a
    slot in the FINAL list and the box's own slot is a legal no-op — the rule
    `moveSlide` already follows), all sharing one `placeableBox()` so the
    slide-level and box-level paths cannot drift into honouring different
    numbers. Box ids come from the store-wide `nextId("text")`: a per-kind
    counter would need its own persistence and its own history watermark, and
    the cost — a fresh slide's id now skips an ordinal — falls on a name nobody
    asserts.
  - **The ceiling is the wire's own number.** `domain::kMaxTextBoxes` is pinned
    EQUAL to `scene::kMaxSceneItems` by test: a ninth box would compile into a
    scene the worker's codec refuses, which is a failure an operator meets while
    presenting rather than while editing. Enforced at `addTextBox`, at
    `fromJson`, and defensively in `compileSlide`, where it is unreachable
    through the model and disclosed as such rather than counted.
  - **`service.json` is format 2.** A slide writes `"items": [{id, lines, x, y,
    width, height}]` in draw order and no longer writes slide-level
    `lines`/`x`/`y`/`width`/`height`. That is a version bump rather than an
    optional key, and the difference is who finds out: an optional key lets a
    v2 file open in an older build, which shows the first box of a slide whose
    author had composed four and says nothing. Format 1 still LOADS and migrates
    to one box whose id is DERIVED (`slide.id + "-text1"`), because a load must
    be a function of its file — loading the same legacy service twice agrees
    about which box is which — and the next save writes that id into the file so
    the migrated identity becomes durable from then on. A v2 slide still
    carrying `"lines"` is refused: `… is format 1 in a format 2 file`.
  - **No item ids on the scene wire.** The compiler emits one wire item per box
    in draw order and the renderer, which has laid out several items since 4.3,
    needs nothing new; the wire's closed key set is not bumped for an identity
    no worker decision depends on. Recorded for 6.17b, where a layer-ordered
    image item may want one.
  - **The session's rules follow from "a copy is a copy".** Rows and the text
    inspector address box 0 until an operator can pick a box (6.17a2); a slide
    with no boxes still reports the DEFAULT rectangle rather than zeros, so a
    canvas drawing the row's outline is never handed nothing; the clip holds the
    boxes with ids CLEARED and a paste re-issues them, which is what lets the
    same slide be pasted twice; pasting a ZERO-box slide removes the box the new
    slide arrived with; the search index covers EVERY box, because a box-0-only
    index tells the operator a show does not contain the credit they typed
    second; and one undo removes a pasted stacked slide whole.
  - **Tests.** `domain_tests`: `exerciseTextBoxes()` (order, per-box edits that
    leave their neighbour alone, unknown ids refusing and naming themselves, the
    cap naming 8, an unplaceable new box refused, the MIDDLE box removed by id
    leaving the outer two in order, drain-to-zero, ids not reused after a
    drain) and `exerciseBoxStorage()` (three boxes' order/id/geometry round
    trip, a re-save byte-identical, a reorder that changes the file, format-1
    migration including "the same derived id on every load" and that id's
    promotion into the next save, nine format-2 refusals, a slide at exactly the
    limit loading, a zero-box slide round-tripping). `scene_tests`:
    `exerciseMultiBox()` (three boxes → three wire items in draw order at three
    rectangles, the slide's style applied to every box alike, the wire round trip
    unchanged, a zero-box slide compiling to no items) and
    `exerciseBoxConstants()` now pinning `kMaxTextBoxes == kMaxSceneItems`.
    `document_controller_tests`: `exerciseMultiBoxSession()` drives a
    hand-written format-2 service — how a stacked slide reaches this build until
    the editor can author one — and checks the row, the inspector, a search hit
    that lives only in the third box, the compiled three-item scene, a stacked
    slide pasted twice with all nine saved box ids verified distinct, and a
    box-less paste that stays box-less. `scene_render_tests`:
    `testAuthoredBoxStackRenders()` compiles TWO boxes from a real
    `domain::Documents` and measures two separated bands each centred in its own
    box, unchanged through the wire, then loads a format-1 file and requires its
    frame to be BYTE-IDENTICAL to a literal transcription of what the
    single-box compiler emitted — "legacy single-box files render identically"
    measured rather than argued. `document_present_tests`: the stacked slide's
    JSON clears a live worker's validator and reports itself on air, the
    box-less slide presents as a scene with no items, and nine items are refused
    at `scene.prepare` naming "over the limit of 8" with the store unharmed
    afterwards.
  - **Mutations** m01–m26 plus m04a/m05a/m10a/m14a: **28/30 CAUGHT**, with blame
    on the intended check (adopting the caller's id failed the
    two-pastes-yield-distinct-ids check, the reversed writer failed the
    draw-order round trip, swapping the two format eras failed the whole package
    load, the shared-geometry compiler mutant failed "none of them inherits a
    neighbour's", box-0-only search failed the credit living in the third box).
    Disclosed: **m19**, `compileSlide`'s own item ceiling, moves nothing because
    the model caps first and the two constants are pinned equal — two libraries
    compiled separately, the same class of truth as 6.14b's pause-drop; **m25**,
    the clipboard clearing the ids it copied, is the second of two identical
    locks, since `addTextBox` drops the id it is handed (m02 shows that one has
    teeth). Two verdicts are reported as they were observed rather than as they
    would read better: **m07** (reorder's off-by-one) was caught by a CRASH in
    the reorder fixture, not by a printed refusal, and **m13** first scored
    "caught (build failure)" because the mutant had swallowed a closing brace —
    rewritten to replace the condition only, it re-scored against a named
    refusal, since a mutant that cannot build is the driver lying rather than a
    fixture passing.
  - Gates desktop **48/48** (229.0 s), headless **27/27** (48.5 s);
    `domain_tests` 0.02 s, `scene_tests` 0.00 s, `document_controller_tests`
    3.73 s, `scene_render_tests` 7.01 s, `document_present_tests` 0.94 s; both
    trees build clean apart from the pre-existing GCC `-Wfree-nonheap-object`
    false positive at `tests/background_media_tests.cpp:2294` (6.15b test code).
- **Task 6.17a2 DONE — the editor authors the stack (review R17, second
  half of the 6.17a split).** Which text box the session is editing is now
  one fact in one place: `DocumentController` holds the selected box's ID and
  resolves every box-scoped question — display or edit — through a single
  `selectedBox()`, id-first with a box-0 fallback, so the canvas highlight,
  the inspector's words, the X/Y/W/H fields and the arrow-key nudge cannot be
  looking at four different boxes. `EditCanvas.qml` draws one outline per box
  in model order (the selected box's outline follows the live drag), puts its
  four handles on the selected box only, and a press on a box body selects the
  TOPMOST box under the pointer (`boxAt`, back-to-front) before the gesture
  starts. `Main.qml` gained New/Delete/Back/Forward (one undo step each) and
  Tab/Shift+Tab walking the stack when nobody is typing. The worker's off-air
  picture renders whatever the model compiles, and a new live leg measures it
  for a stack against its own minus-one-box twin: mean luminance 2.59 -> 4.17
  through the real shared preview stream, printed every run.
  - **Selection is chrome, and chrome has rules.** It dirties nothing and
    triggers no autosave — a click is not an edit — but it FLUSHES the
    debounced keystrokes first: a pending burst belongs to the box it was
    typed into, not the box the operator tabbed to. It announces itself on
    `boxSelectionChanged` + `slideTextChanged` only; `documentsChanged` would
    repaint the workspace and re-ask the worker for a click.
  - **The id rules, with two deliberate exceptions.** The selection follows
    its box across reorder and undo, and `removeCurrentTextBox` deliberately
    LEAVES the id (resolving to nothing meanwhile) so an undo — which restores
    the exact snapshot, ids included — puts the selection back on the very
    box the operator deleted, which is where they are looking. Navigation to
    another slide instead CLEARS it: the selection belongs to a slide, and a
    carried id that silently re-selects an old box on the way back is a trick
    nobody asked for.
  - **The handle-anchor trap, found before it shipped.** A corner handle of
    the selected box can lie over ANOTHER box; had a press there re-selected
    by topmost hit, the resize would stay anchored to the old box's opposite
    corner and commit onto the newly selected one. Body presses re-select;
    handle presses never do.
  - **Tests.** `exerciseBoxAuthoring()` (overlap hits the box on top, clamps,
    box-less refusals naming themselves, the flush-before-move, the signal
    pair without `documentsChanged`, the dirty-free click measured on a
    session that starts clean, add/remove/move each exactly one undo step
    with the id snap-back, eight boxes filling a slide and the ninth refused
    naming the limit, and the refusal leaving no history step — checked by
    watching the last TYPED name revert) and `testStackedBoxesOffAir()` in
    the live suite (the stacked scene selected off-air against its own twin,
    so the only difference between the two measured frames is the box under
    test).
  - **Mutations** m01–m17: **17/17 CAUGHT** — after three blind spots were
    found and fixed mid-run, which is part of the result: the dirty-check
    compared against a session that had RECOVERED autosave and was dirty
    before the click; the navigation-clear left an id-overwriting add between
    the crossings so a carried id still fell back to 0; and the first m16
    mutated `compileSlide` while running the live suite alone — whose legs
    prepare WIRE scenes and never call the compiler. Retargeted (m16 at the
    renderer, caught with the stacked live frame going mean-identical at
    2.593368 -> 2.593368; m17 the compiler against the payload checks,
    caught), all four re-scored to named failures. m06 (the box-less-slide
    guard) is caught by a CRASH, verdict real and message not. Disclosed
    blind: the `boxSelectionChanged` emits in undo/redo/load (their consumer
    is QML chrome; the selectTextBox pair IS counted), `boxAt`'s exact-edge
    inclusiveness, and the gesture wiring inside the two QML files — the
    load gate covers loading, and the invokables behind every gesture are the
    mutated surface.
  - Gates desktop **48/48** (228.3 s), headless **27/27** (48.5 s);
    `document_controller_tests` 3.74 s, `scene_render_tests` 6.95 s,
    `preview_selection_tests` 14.49 s; both trees build clean apart from the
    pre-existing GCC `-Wfree-nonheap-object` false positive at
    `tests/background_media_tests.cpp:2294` (6.15b test code).
- **Task 6.17b1 DONE — a picture in the stack: model, file, wire and worker
  draw (review R17, first half of the 6.17b split).** `TextBox` became
  `SlideItem` with a `kind` and an `assetId`, and the ceiling became
  `kMaxSlideItems` — ONE budget of 8 for both kinds, pinned equal to
  `scene::kMaxSceneItems` by test, because what bounds a composition is the
  scene message and not the kind of the thing being added. The kind is what
  says which content fields mean anything, so the model refuses a half-item at
  the door naming its rule: a picture names an asset that EXISTS and whose
  name the media pipeline reads as a still, and carries no words; a text item
  naming an asset is refused; `setItemLines` refuses a picture; and
  `setSlideLines` — the legacy view that addresses item 0, which can now be a
  picture — refuses rather than write words INTO the image or invent a box
  underneath it.
  - **One video test, not two.** The predicate the model refuses with is
    `domain::assetPathLooksLikeVideo`, and `scene::assetPathIsVideo` now
    delegates to it: the name list that classifies a background and the rule
    that rejects a video in the foreground are the same list, so they cannot
    drift apart, and a fixture that rebinds a placed picture onto `.mp4`
    reaches the compiler's own guard through the real path (task 6.12's
    adoption), naming the item.
  - **Deleting an asset takes its pictures with it.** A background degrades to
    none; a stack entry whose file is gone is not a half-drawn item but a claim
    about the composition the worker could never honour, so the item leaves
    with the asset — and because History holds whole states, the undo restores
    the asset AND the item, with the item's own id in its own slot, rather than
    a slide coming back with a hole in the middle of its stack.
  - **Format 3, and why text's kind is written too.** `kServiceFormat` 2→**3**:
    the kind rides EVERY item (leaving text's to inference is how a future
    default would silently redefine what an old file drew), the key set is
    closed PER KIND, and formats 1/2/3 all read. Seven hand-written lies are
    refused naming themselves — unknown kind, missing kind, a dangling asset
    reference, words on an image, an assetId on a text item, an empty asset
    reference, and a `"kind"` in a format-2 file, where that key does not exist
    and its presence means somebody else's build meant something by it.
  - **Scene format 2 and the same gate for both paths.** `SceneItem` carries
    `isImage/assetId/assetPath`; `kSceneFormat` 1→**2** with no tolerance in
    either direction (a scene-1 payload is refused rather than drawn with
    whatever kinds an older build meant); a picture's `assetPath` goes through
    the background's own `assetPathIsLocal`, so a `data:` URI is refused as the
    payload it is and a path the worker would have to fetch as the network it
    has no contract for. Protocol minor 9→**10** plus `scene.image-items.v1`
    (13 capabilities).
  - **The draw: array order, the authored box, and an honest absence.**
    `drawLayout` walks the stack in the order the model gives, so image-over-
    text and text-over-image are the operator's choice and nobody else's; a
    picture is STRETCHED into its authored rectangle (a native-size draw leaves
    60 device pixels of background in the middle of a 120x80 plate, a
    contain-fit bars the top and bottom); the lookup stays the 6.10 rule — a
    picture the session has not decoded is simply not drawn, measured equal to
    the composition WITHOUT the item at worst 0/255; and the fade's alpha rides
    the image paint, no render-to-texture, measured g127 at half strength with
    the steady frame at g0 and the fill outside the box at g255.
  - **The session decodes, pins and pumps.** `preloadScene` refreshes each
    foreground path on the RPC thread and names every failure on stderr while
    the old scene is still on air; the MEDIA lane deliberately stays
    background-only, because `status.media` names what the audience's transport
    is showing and a stack picture's decode error would evict that truth with a
    file nobody is playing (per-item fault reporting belongs to R19's media
    lane, not here). `syncScenePins` unions the foreground paths under the same
    distinct-path rule (measured: 0 → 1 pins across one prepare of a plated
    slide). `ThumbnailJob` grew a path VECTOR with settled flags: **a row's unit
    of debt is the row, its unit of work a path**, so a two-picture slide is
    ONE frame and arrives only when both settle — measured r134 g142 b142,
    where the first-settle-draws mutant reads r239 g222 b46: pure backdrop.
  - **`pasteClip` rewritten, because the legacy view cannot address a
    picture.** The inserted default box is removed and every clip item is
    re-added through `addItem`, which issues the fresh identity and enforces
    the kind rules; a picture whose asset vanished falls away exactly as a
    deleted style or background reference has since 4.5. `currentSlideBoxes`
    gained `kind` and `assetId` for the 6.17b2 chrome.
  - **Tests.** `exerciseImageItems()` (kinds and every refusal, a mixed slide
    filling to the ceiling, `removeAsset`'s drop with the surviving order, the
    undo restoring both, the legacy-view refusal, the video predicate); the
    format-3 round trip with byte-stable re-save, `sameService()` now comparing
    kind and asset, the seven hand-written refusals and an honest hand-written
    package; `exerciseImageWire()` (the compiler's asset→path resolve, per-kind
    wire keys with no typography on a picture, round trip, and the refusals
    including the compile-time video guard reached by rebinding a placed
    picture onto `.mp4`); `testForegroundImageItems()` and
    `testForegroundImageFade()` in the Skia suite; and in the live suite a
    two-picture row (one frame for two decodes, the pool holding exactly two
    pictures at thumbnail scale, a re-ask costing nothing) plus
    `testForegroundImageOffAir()` (mean 0.00 → 84.50 with the plate over half a
    black canvas) and the pins delta above.
  - **Mutations** m01–m37: **37/37 CAUGHT**, each on the check named for it —
    the discipline, both word-writing views, the drop, the writer's kind, five
    reader refusals, the compiler's resolve and guard, both wire key sets, kind
    defaulting, the reference gate, scene-1 tolerance, the steady draw, the fade
    alpha, the stretch, layout dropping a picture, the snapshot's up-front load,
    prepare-time decoding, first-settle-draws, the pin union, paste losing its
    pictures, the canvas's kind and asset, and the capability. Two mutants were
    scored twice: the first run's anchors for the fade and the paste had gone
    stale (a re-indent, a dropped `.empty()`), so the driver's dry run now
    preflights every anchor and both re-scored against named refusals — a
    mutant that cannot run is the driver lying, not a fixture passing.
    Disclosed blind: `preloadScene`'s choice of `refreshFor` over `imageFor`
    (needs a file missing at one prepare and back at the next, and a foreground
    decode failure is a stderr line with no status to make false — the REPORTED
    equivalent is the media lane's, tested in `background_media_tests`); the
    job's path DEDUP and the lookahead accounting inside one job (no fixture
    names one file as both a background and a foreground, so a duplicate settles
    for free); `pasteClip`'s fresh identity, which is `addItem`'s to mutate and
    6.17a1's battery already scores; and the two QML files, whose use of the new
    canvas fields is 6.17b2's to write — `ui_loads` proves they still load.
  - Gates desktop **48/48** (230.6 s), headless **27/27** (48.8 s); `domain_tests`
    0.02 s, `scene_tests` 0.00 s, `protocol_tests` 0.01 s, `document_controller_tests`
    3.48 s, `scene_render_tests` 7.21 s, `worker_rpc_limits` 11.64 s,
    `preview_selection_tests` 15.28 s; both trees
    build clean apart from the pre-existing GCC `-Wfree-nonheap-object` false
    positive at `tests/background_media_tests.cpp:2294` (6.15b test code).
- **Task 6.17b2 DONE — the operator places pictures; the stack stays one
  (review R17, second half of the 6.17b split).** `DocumentController::
  placeAssetOnSlide` is the drawer's door: the selected asset joins the current
  slide as a TOPMOST image item and the selection goes with it — the picture
  analogue of `addTextBox`, through `edit()`, so one undo removes one
  placement. Every content rule stayed the DOMAIN's: `addItem` still owns the
  unknown-asset, video and ceiling refusals and the status line quotes it, so
  the panel cannot grow a second opinion that drifts from the model's. The
  arriving box is a centred half-canvas (480,270,960,540) rather than the lyric
  default: a full-frame picture coincides exactly with the default outline and
  reads as "nothing happened".
  - **The mixed stack is one, measured not assumed.** Outlines, handles,
    `boxAt`, Tab, nudge, `setTextBoxGeometry`, Back/Forward, Delete and
    undo/redo all resolve through `selectedBox()` and item ids — the 6.17a2
    machinery was already kind-blind — and the fixture walks it: a picture
    moves behind its verse and back, dies and rises through undo with the
    selection following its id onto it at its own index, and navigation clears
    it exactly as it clears a text selection. What placement adds to that
    machinery's honesty is its own `flushText`: a burst still in the debounce
    when a placement lands commits into the box it was TYPED into, instead of
    the new selection carrying it onto the picture — where the refusal guard
    would then honestly drop it, which is data loss with good manners.
  - **Refusals out loud, and a field that isn't there.** `commitPendingText`
    names a selected picture on the status line instead of swallowing the burst
    into a `setItemLines` refusal nobody can see (measured against a fresh
    save: the words reach neither box and `dirty` stays false). The QML
    inspector hides the words field on an image selection — `currentBoxKind`
    ("text"/"image", empty with no slide) and `currentBoxAsset` (an
    `assets()`-shaped map) are the two new views it reads — so the usual path
    never invites the refusal; the controller guard stays as the backstop for
    every other caller.
  - **Mutations** m01–m11: **11/11 CAUGHT**, each on the check named for it —
    kind always text, kind answering with nothing selected, the asset map
    emptied, the name row carrying the id, the swallowed refusal, a placement
    without its flush, one arriving as text, one on the lyric default box, one
    bypassing History, one that does not select, and the no-slide guard
    deleted. Disclosed blind: the placement's two emits (headless reads are
    live; they exist for the QML bindings, and QML chrome is the blind spot —
    `ui_loads` proves the drawer button and `pictureInfo` compile and
    instantiate offscreen but no harness drives them); `currentBoxAsset`'s
    missing-asset fallback, unreachable through this controller since
    `removeAsset` drops the items and `loadService` refuses dangling
    references; and the QML visibility and double-click expressions themselves.
  - Gates desktop **48/48** (228.2 s), headless **27/27** (48.9 s);
    `document_controller_tests` 3.48 s, `ui_loads` 8.03 s; both trees build
    clean apart from the pre-existing GCC `-Wfree-nonheap-object` false
    positive at `tests/background_media_tests.cpp:2294`. One gate run took a
    single display-side failure that passed on rerun (11.80 s, consistent with
    `worker_rpc_limits`) and did not recur across two further full runs.
- **Task 6.17c1 DONE — the slide chooses how its picture fits; contain
  letterboxes on screen (review R17, first leaf of the 6.17c split).**
  `Slide::backgroundFit` (default COVER, the worker's rule since 3.2, so
  every pre-4 file re-renders pixel-identical) is authored through
  `Documents::setSlideBackgroundFit`, and the enum's ONE name pair
  `backgroundFitName`/`backgroundFitFromName` lives in the DOMAIN — the
  video-predicate pattern — so no layer grows a private list of what a fit
  is called; `fill` is deliberately not in the set. `service.json` is
  format **4** (`kServiceFormatLegacyUnfit = 3`): `"fit"` is written on
  EVERY slide, cover included, byte-stability and format-3-promotion
  pinned; formats 1/2/3 load as cover (their own history), an unknown name
  is refused quoting itself, and a hand-bumped `"fit"` in a pre-4 file is
  refused (`where that key does not exist`). `kSceneFormat` 2→**3** with
  `background.fit` MANDATORY and no tolerance either direction — scene-1
  AND scene-2 payloads are refused by the scene-3 reader — which is what
  keeps the split honest: a worker that predates 6.17c2 cannot misdraw
  `"blur"`, it rejects the scene naming it. Protocol minor 10→**11** +
  `scene.background-fit.v1` (14 capabilities). The worker splits `fitDest`
  by the scene's choice in BOTH paths — cover unchanged (the 3.2 fixtures
  pass untouched), contain min-scale centred with the fill showing in the
  bars — and the R13 fade group keeps its single composite per region:
  bars via a clip-difference fill at the group alpha, the picture's rect
  via the same `picture over fill` shader, disjoint so no pixel counts
  twice and no render target enters the pass. The split was not free: the
  shipped all-canvas shader had been clamping the picture's edge pixels
  beyond its dest rect — invisible for three milestones because cover's
  dest covers the canvas — measured **92/255** on the first contain fade
  fixture and fixed to **0/255** over the whole frame; the cover-era cost
  is unchanged (fade 36211 us/frame vs the two draws it replaced 42416 at
  1920x1080, well under the 4x gate). Controller: `currentBackgroundFit` +
  `setSlideBackgroundFit(QString)` refusing unspeakable names BEFORE the
  edit seam (the no-history-entry claim is provable: one undo still
  reverts the fit), `slides()` rows carry the fit, the clipboard carries
  and pastes it, and the style panel gained the BACKGROUND FIT combo.
  Raw-payload fixtures across seven desktop test files moved to scene 3 +
  explicit `"fit"` as the wire tightened.
  - **Mutations** m01–m18: **18/18 CAUGHT** (`/tmp/opencode/battery617c1.py`)
    — default flipped, the name pair swapped, `blur` parsing quietly, the
    writer leaving cover to inference, the writer lying (`cover` always),
    both storage refusals deleted, the scene number un-bumped, a missing
    wire fit defaulted, any wire name accepted, the compile dropping the
    slide's choice, equality ignoring the fit, each `fitDest` scale
    crossed, the fade group re-clamping the picture into the bars, the
    controller's name-fallback, the paste dropping the fit, and the
    capability unannounced. Disclosed blind: the combo's binding and
    `stylePanel.sync()`'s fit line (chrome — `ui_loads` proves the combo
    compiles and instantiates offscreen); the legacy format-1/2 defaulting
    shares the pinned format-3 path; the bars/dest seam is measured, not
    separately asserted (the whole-frame lerp scan would catch one).
  - Gates desktop **48/48** (229.17 s), headless **27/27** (48.9 s); both
    trees build clean apart from the accepted `-Wfree-nonheap-object`
    false positive and the five whitelisted QML notices.
- **Task 6.17c2 DONE — blur draws the reference's backdrop, built once per
  image, faded as one group.** `"blur"` joined the domain's ONE name pair
  and nothing else moved: service format stays 4, scene format stays 3, the
  key stays `fit` — the closed-name refusals c1 shipped are exactly what
  makes this widening honest (an old build rejects `"blur"`, it cannot
  misdraw it), and `scene.background-fit-blur.v1` + minor **12** (15 caps,
  pinned in both hello fixtures) is what a frontend checks before offering
  it. Blur+video — the pairing the draw cannot honour — is refused at all
  three gates that see both halves: the controller seam (before the edit
  seam, no history entry: `this build does not blur playing frames`), the
  compile, and the wire (a hand-built payload drawing contain while
  claiming blur is a lie scene 3 refuses). The backdrop is derived image
  data: `SceneImageCache::backdropFor` cover-fills with a 24 px overscan
  (no darkened frame edge), blurs sigma 6 with the 0.3 baked into its
  alpha, ONCE per image off-lock at the first blurred draw, memoised in the
  entry, its bytes counted into the budget — eviction, invalidation and
  install-over each subtract them, and it dies with its image. `fitDest`'s
  test is now `Cover ? max : min` so blur's PICTURE contains. The fade
  keeps R13's no-layer rule by nesting — bars `Blend(fill, backdrop)` over
  the clip-difference, dest `Blend(picture, Blend(fill, backdrop))`.
  Measured: backdrop build **83 ms** at 1920x1080 vs **20 ms** half-size,
  worst-channel difference **3/255** on the banded fixture — full-res
  ships because the build is paid at the slide change, not at 60 Hz, so
  the approximation buys nothing the budget needs; all three fade fixtures
  (fill→blur 0.5, self-fade 0.5, blue-fill→TRANSLUCENT blur 0.75) measure
  **worst 0/255, 0 px over tolerance** across the whole frame.
  - **Mutations** m01–m20: **20/20 CAUGHT** (`/tmp/opencode/battery617c2.py`)
    — the three name functions, both video refusals, the capability,
    sigma/alpha/overscan, the backdrop fetch, the steady draw, both fade
    regions, the memo store, all three byte-accounting sites, both
    controller-guard directions. Two honest mid-run findings: the literal
    drop of the caller's blur→contain conversion is an EQUIVALENT mutant
    (fitDest tests only Cover, so Blur already contained — disclosed as
    SURVIVED on first run, m11 re-aimed to forcing COVER at the
    conversion, which moves pixels and is caught); and the first-run m14
    survival found a real gap — an opaque SrcOver picture ignores its dst
    shader, so the fade's dest NESTING is only observable through a
    TRANSLUCENT picture, now a pinned fixture (blend 0.75, where the
    missing backdrop reads ~12/255). Disclosed blind: the QML combo's
    third-entry binding and sync index (chrome; `ui_loads` proves it
    instantiates), the minor bump itself (hello accepts any minor — the
    capability is the enforceable gate), the worker's `video == nullptr`
    guard (defence in depth behind the pinned wire refusal).
  - Gates desktop **48/48** (231.73 s), headless **27/27** (50.75 s); both
    trees build clean apart from the accepted `-Wfree-nonheap-object`
    false positive and the five whitelisted QML notices.
- **Task 6.17c3 DONE — the background is a pair, and every fill is one
  shader.** `Style::backgroundGradient` (`""` = flat, what every file ever
  written drew) lives on the STYLE because `backgroundColor` always did —
  the fit went onto the slide for the same reason a choice belongs to the
  thing it describes. `createStyle`/`updateStyle` took the parameter with
  NO default, so the compiler makes every caller state it rather than let
  a rebuild flatten a gradient someone authored; a bad colour is refused
  naming its role (`background gradient colour 'dusk' is not #rgb or
  #rrggbb`). Service format **4→5**: the writer emplaces the key on every
  style, flat included — `""` IS the statement, so re-saves are byte-stable
  and a format-4 re-save PROMOTES its flat rather than trusting an absent
  key; formats 1–4 read flat (their own history), a format-5 style missing
  the key is refused (the writer always writes it, so absence is a
  hand-edit), and a hand-bumped key in a pre-5 file is refused naming the
  format where it does not exist. The colour is not validated at read,
  exactly as `backgroundColor` never was — the compile refuses once, naming
  the style. Moving the format number exposed a latent branch: the slide's
  `fit` test was `format == kServiceFormat`, which would have begun
  refusing format-4 files for a key they legitimately carry, so it is
  `format >= kServiceFormatLegacyFlat` now (m18 pins it). The wire keeps
  **scene format 3** and `"gradient"` is its ONE optional background key —
  written only when set, so a flat payload stays byte-identical to what
  this build wrote yesterday, and a worker older than minor **13** refuses
  a payload carrying it BY NAME rather than drawing it flat;
  `scene.background-gradient.v1` (16 caps, both hello pins) is the check
  before sending one. Equality gates the second colour on the flag: flat
  under a gradient is a new picture, two unflagged junk values are one.
  The worker has ONE fill now — `backgroundFillShader()`, a colour or a
  `LinearGradient` (0,0)→(0,1080) riding the CTM — replacing the colour-only
  draw, the fade's letterbox bars, the fade's dest under-layer and the
  steady fill, so a gradient can never meet a flat fill mid-composite. It
  interpolates in gamma-ENCODED sRGB, the browser's rule: for
  `#102030`→`#E09040` the midline is the byte mean R120/G88/B56, where a
  linear-light ramp gives R165/G107/B57 — 45 units of red between the two
  promises, so the midpoint fixture pins a doctrine, not a hope.
  Measured: bare-canvas endpoints and the exact `#785838` midline, one row
  = one colour, a monotonic column, and the flat regression; contain's bars
  grade (top vs bottom differ >40, the picture's thirds unmoved); blur's
  bars are `blendOver(ramp(row), red, 77)` to the channel with the ramp
  still visible under the backdrop; the three fades (flat→contained graded
  0.5, graded→itself 0.5, TRANSLUCENT graded→itself 0.75 — c2's lesson, an
  opaque SrcOver picture hides its dst shader) all measure **worst 1/255,
  0 px over tolerance**. `editStyle` states the colour, `currentStyle()`
  carries it, `createStyleFromCurrent` forks it with the slide, and the
  panel grew `styleBackGradientField`; the edit canvas and thumbnails show
  the ramp through the worker's own preview, so no second renderer joined
  them (demo.cpp stays flat — its fixtures pin shipped pixels).
  - **Mutations** m01–m22: **22/22 CAUGHT** (`/tmp/opencode/battery617c3.py`)
    — the flat guard, both axis mutants, linear-light interpolation, all
    three flat-fill-pinning sites, three wire mutants (key when flat, key
    mandatory, key set forgetting it), three compile/equality mutants, the
    capability, four storage mutants (writer omits, pre-5 trusted, format-5
    defaultable, fit branch regressed), both domain refusals, both
    controller seams. Disclosed blind: the minor 12→13 bump itself (hello
    accepts any minor — the capability IS the enforceable gate), the QML
    field and its `sync()` binding (chrome; `ui_loads` proves it
    instantiates, and the data it reads/writes is mutated at the
    controller), and `wantString`/`wantColor`'s discipline on the new key
    (unchanged helpers, pinned by every older key they read).
  - Gates desktop **48/48** (231.84 s), headless **27/27** (51.37 s); both
    trees build clean apart from the accepted `-Wfree-nonheap-object`
    false positive and the five whitelisted QML notices.
- Exact next action: **6.18a** per the PLAN.md ledger (review R1/R4/R5/R10/
  R13) — extract `SceneStore`/command-state and `PreviewProducer` from
  `OutputSession` now that their correctness fixes are in: the session
  coordinates the loop and delegates to single owners with explicit thread
  rules, regression gates unchanged, and no second mutable scene truth is
  introduced by the split.

## Historical M1 checkpoint (superseded by the current checkpoint above)

- Milestone: **M1 — Prove frontend-to-renderer integration**, tasks **1.1**
  through **1.4** DONE (all 2026-10-22/23). M0 was closed earlier with
  tag `M0` at `bca7cee`; commits and tags are delegated to the agent for
  this session.
- Task 1.4 delivered shared-memory previews and was **accepted on
  screen**: the operator pane streams the same Vulkan-composed scene the
  audience sees, and a stalled preview consumer never touches the
  output.
  - **1.4a** `protocol/preview_wire.h`: one POSIX shm segment per
    stream (`/openshow-preview-<pid>-<generation>`, magic `OVSP`,
    layout v1) = 64-byte segment header + three slots of 64-byte
    seqlock header + RGBA8888 payload; `makeNotification` added for
    `preview.ready` / `preview.released`. One Qt-free definition
    compiled by both processes (protocol tests pin the geometry).
  - **1.4b** `worker/preview/PreviewStream` (Qt-free producer) driven
    by `OutputSession`: at 15 fps the *same* `TestScene::draw()`
    renders into a 640x360 `SkSurfaces::RenderTarget` on the output's
    own `GrDirectContext` — identical backend to the audience surface —
    and `SkSurface::readPixels` lands RGBA8888 straight in a claimed
    slot. Slots are only claimable once their last publish was
    released; when none qualifies the frame counts as dropped and the
    output never blocks.
  - **1.4c** RPC: `preview.start {width?,height?,fps?}` /
    `preview.stop`, worker→UI `preview.ready {slot,seq,rowBytes,…}`,
    UI→worker `preview.released`, capability `preview.shm.v1`, status
    `preview {running,name,width,height,fps,published,dropped,error}`;
    notifications drain on every 50 ms poll tick. Pixels never cross
    the socket.
  - **1.4d** `WorkerController` maps the named segment on the **first
    ready**, not on the reply (the reply names a segment the render
    thread has not created yet), copies under the seqlock with the
    sequence re-checked around the memcpy, and always releases;
    `PreviewView` aspect-fits frames into the pane. Seen live:
    "preview · 77 frames shown · 0 dropped".
  - **1.4e** ctest scenario D inside `worker_rpc_lifecycle`: geometry
    echo (proves `jsonUint`), header/size agreement, two seqlock-intact
    non-blank copies, content follows an images-layer toggle (>5% of
    pixels differ), all three slots rotate, a stalled consumer raises
    drops with `published` frozen while the output keeps presenting,
    releases resume publishing, stop unlinks the segment, restart gets
    a fresh name at the documented 640x360 default.
- Task 1.3 delivered display control over the wire and was **accepted on
  screen**: the worker now *starts* on the second panel automatically (the
  FreeShow autoPosition rule; `--monitor` remains only as a development
  override), and RPC moves it.
  - **1.3a** `OutputWindow::setMonitor(index, settleMs)` carries the
    map-then-fullscreen + configure-pump sequencing and serves both open()
    and later moves; `OutputSession` grew a desired/applied monitor pair
    applied at the top of the frame exactly like the layer mask, a
    `DisplayInfo` snapshot refreshed at open, after each move and every 60
    frames (hotplug reaches `output.displays` without monitor callbacks
    yet — the dedicated story stays M6.1), and `Status::monitor/
    monitorName`.
  - **1.3b** RPC `output.displays` + `output.select {monitor:-1|N}`
    (validated against the snapshot on the RPC thread; bad/absent index is
    a structured invalid-params error); `output.status` gained `monitor`
    and `monitorName`.
  - **1.3c** the UI has a Destination dropdown (Windowed + panels with
    modes) bound to the *applied* state; `WorkerController` fetches
    `output.displays` after hello and tracks `currentMonitor`.
  - **1.3d** automated: ctest scenario `worker_rpc_lifecycle` now also
    enumerates displays, refuses bad selections and waits for status to
    confirm monitor 0 then windowed. Seen: `/tmp/opencode/switch-sheet.png`
    shows the four states — DP-4 fullscreen, eDP-1 fullscreen (2256x1504),
    1280x720 windowed, back on DP-4 — driven purely over the socket.
    **Interactive drag-resize remains a manual check** (the recreate path
    it exercises is the same compositor-configure one proven repeatedly,
    but Wayland cannot script the drag; re-check when convenient).
- Task 1.2 delivered in the five subtasks PLAN.md's graduation note demands,
  and **accepted on screen**: the Qt frontend spawns
  `openshow-worker --socket PATH`, handshakes over JSON-RPC 2.0, and drives
  the output; closing the UI shuts the worker down.
  - **1.2a** `session/OutputSession` is the runtime seam: it owns the window,
    Vulkan device, Ganesh context, swapchain and `TestScene`, and runs the
    frame loop that used to live in `main.cpp`. It exposes no window, device
    or swapchain handle. `--render-test` is now a thin driver over it and
    presents identically (same 1728x972 / native 1920x1200 extents, same
    readback ratio).
  - **1.2b** `openshow::protocol` gained the wire: 4-byte little-endian
    length + JSON framing with a 1 MiB cap, an incremental `Decoder` that
    poisons the connection on a framing violation, JSON-RPC 2.0 envelopes
    and the `hello` version negotiation.
  - **1.2c** `rpc/RpcServer` serves one frontend from its own `poll()`
    thread (50 ms ticks) on a raw `AF_UNIX` socket. Nothing is answered
    before `hello`; the worker unlinks a stale path before binding.
  - **1.2d** live commands: `layer.setEnabled` mutates a guarded desired
    mask that the **render thread applies at the top of a frame** and
    reports through an acknowledged `revision`. Proven on DP-4: two socket
    commands made the photo and title vanish, and a re-enable brought them
    back (`/tmp/opencode/toggle-off-dp.png`).
  - **1.2e** `ui/WorkerController` spawns the worker, retries the socket
    until it answers, runs the handshake, polls `output.status` at 2 Hz into
    QML (worker state in green, device, frame and revision counters, five
    layer checkboxes) and forwards the worker's log.
- Both required exits work and are automated: a served `shutdown` and the
  frontend simply going away each leave `openshow-worker` exit 0 with the
  socket unlinked (`/tmp/opencode/live.log`, ctest `worker_rpc_lifecycle`).
- Rendering is built on the **output layer model** (PLAN.md): every frame is
  composited bottom-to-top from background, images, words, aux and clock in
  a single pass with no render-to-texture; disabled layers are skipped
  without dispatch. ctest `render_layer_composition` verifies this on the CPU.
- Checks: `ctest --preset desktop` passes **8 of 8** (`worker_rpc_lifecycle`
  now runs four scenarios including the preview stream, labelled
  `display`); `ctest --preset headless`
  passes **6 of 6** via the test preset's
  `filter.exclude.label: "display"` — note the earlier "6 of 6" for task
  1.2 was a stale build dir, the filter as first written (`labelExclude`)
  was silently ignored; the working key is `exclude.label` (1.3 fixed it
  and re-verified with `--show-only`); `worker_is_qt_free` stayed green
  throughout, so the RPC path is `poll()`/Boost.JSON with no Qt.
- Task 1.5 (video decode + A/V benchmark) is **IN PROGRESS** with the
  approved subtask rows 1.5a–1.5f in PLAN.md: FFmpeg software decode to
  native I420 → bounded staging ring → device-local optimal-tiling plane
  ring on the *transfer* queue family (upload thread: copy +
  `TRANSFER_DST→SHADER_READ` + fence) → one
  `SkImages::TextureFromYUVATextures` wrap per slot, PipeWire callback as
  the clock, and an event-driven presentation scheduler. Verified
  against the pinned Skia `a94df1cdab` and this machine's vulkaninfo:
  the old `SkExternalImageInfo` is gone; the seam is
  `GrYUVABackendTextures` + `SkImages::TextureFromYUVATextures`
  (`include/gpu/ganesh/SkImageGanesh.h:257`), wrapping trusts
  `GrVkImageInfo::fImageLayout/fCurrentQueueFamily/fSharingMode`,
  `setMutableState` is the client-side update hook; RADV STRIX1 samples
  R8/R8G8/2-plane/3-plane YUV with optimal tiling, and queue family 1 is
  compute+transfer with 4 queues (a real async upload path).
  **1.5a DONE**; next is 1.5b (decode thread + FrameRing).
- Next: task **1.5b**, and one manual
  check to close 1.3 fully: drag-resize the borderless window on screen and watch
  the swapchain recreate (the same code path a compositor configure drives,
  already proven — this is the acceptance row's honest caveat).

## Completed work

- Renamed the documentation from the working title FreeShowQt to OpenShowQt and
  rewrote `README.md`. Upstream FreeShow references are untouched.
- Checked the Git remote, its reachability and the ignore rules (task 0.2).
- Replaced SDL3 with GLFW plus vk-bootstrap for the worker's output window and
  native PipeWire for audio, after inspecting `/home/jim/sloppy-ui`.
- Dropped Asio and nlohmann/json in favour of a `poll()` loop and Boost.JSON.
- Recorded dependencies, licences and the Skia findings (task 0.5).
- Added the build skeleton (task 0.4):

| Path | Product |
|---|---|
| `CMakeLists.txt`, `CMakePresets.json` | top level, `desktop` and `headless` presets |
| `cmake/OpenShowSkia.cmake` | `openshow::skia` from a GN-built Skia tree |
| `src/domain` | `openshow::domain`, Qt-free, version only so far |
| `src/protocol` | `openshow::protocol`, Qt-free, protocol constants and a Boost.JSON offer |
| `src/worker` | `openshow-worker`, Qt-free, `--check-deps` reports linked libraries, `--socket` returns exit 3 as not built yet |
| `src/ui` | `openshow-ui`, Qt 6 Quick window with placeholder text |
| `tests` | domain and protocol tests, an `ldd` based Qt-dependency guard, worker `--check-deps` |

- Derived `docs/reference.md` from the FreeShow source for task 0.3: shell and
  panel geometry, theme tokens, pages and drawer tabs, the show page playlist and
  slide grid, the full keyboard map with its gating rules, output and compositing
  semantics including background continuity, the saved data model and storage
  layout, and a mapping from FreeShow's operation surface to our planned RPC.
- Confirmed the FreeShow renderer is **Svelte**, not Vue, and corrected the
  earlier note here. 547 `.svelte` and 466 `.ts` files under `src/`.
- Verified that document against the running app and fixed what it had wrong:
  resize snapping is 100.05 / 200.1 / 290±20 capped at 638, not the numbers first
  transcribed from source; `F5` advances the output instead of clearing timers, and
  the per-layer timer is cleared by its own button; the drawer search is a
  right-aligned 290x36 field that only exists while open, not a permanent
  `--navigation-width` field; store JSON lives in `<dataPath>/Config/`, and a fresh
  install has digit-to-slide and letter-to-slide **off** by default. Things only the
  running app showed: the right panel above 522px becomes
  `flex-direction: row-reverse`, clearing leaves pure black through an 800ms
  `background-color` transition on the slide box, output scaling is literally CSS
  `zoom = boxWidth / 1920`, the top 5% of an output window is an
  `-webkit-app-region: drag` strip, an off-screen output window keeps rendering
  live DOM, `Esc` with a selection never clears the output because the presenter
  path fires at 0ms against a 20ms deselect, and the serialised `out` buffer is
  persisted inside the output object so a restart re-presents what was on air.

## Task 1.1 log (2026-10-22)

New worker sources: `src/worker/output/OutputWindow.{h,cpp}`,
`VulkanContext.{h,cpp}`, `Swapchain.{h,cpp}`, `src/worker/render/Layer.{h,cpp}`,
`render/TestScene.{h,cpp}` and a rewritten `main.cpp` with
`--render-test [--monitor N] [--seconds S]`, `--snapshot PATH`,
`--list-monitors`, `--check-layers` and repeatable `--disable-layer` /
`--enable-layer` toggles. Findings, in the order they bit:

- **GLFW on Wayland:** `glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_WAYLAND)`
  with an `ANY_PLATFORM` fallback; confirmed "GLFW platform wayland" at
  runtime. `--list-monitors` saw both panels while DP-4 was attached
  (`eDP-1 2256x1504@60`, `DP-4 1920x1200@60`, content-scale 2.0).
- **ICU mismatch:** the prebuilt `libskunicode_icu.a` wants ICU v77 versioned
  symbols; this Fedora has 78. Decision: use the hard-coded SkUnicode backend
  plus a compat `SkUnicodes::ICU::Make()` shim (libskshaper calls it
  unconditionally). Full-ICU resolution is deferred to M3, together with
  proper line breaking and bidi.
- **Line-break flags:** my first `computeCodeUnitFlags` placed the hard-break
  flag *on* the newline; ICU's UTF-8 path puts it on the position *after*,
  which silently swallowed one character per line ("OpenShowQ", "SkParagrah").
  Fixed and verified by snapshot: Greek, em dash and accented text all render
  with no dropped characters and no tofu.
- **Skia Vulkan backend context (MakeVulkan returned null):** this Skia
  revision *requires* `fMemoryAllocator`
  (`skgpu::VulkanMemoryAllocators::Make` from the private header, as
  sloppy-ui does) and wants `fVkExtensions` and `fDeviceFeatures2` populated
  from vk-bootstrap's enabled device extensions/features. Instance
  extensions are not checked (this vk-bootstrap has no
  `Instance::get_enabled_extensions`; pass 0/nullptr like sloppy-ui).
- **Wayland surface extent:** immediately after fullscreen,
  `currentExtent` is `0xFFFFFFFF` (undefined until the compositor configures
  the surface). The swapchain now falls back to the GLFW framebuffer size and
  relies on the loop's OUT_OF_DATE recreate; the frame loop also recreates
  whenever the framebuffer size changes.
- **`WrapBackendRenderTarget` returned null for every image:** the cause was
  *not* the format pairing — diagnostics printed `format 37
  (B8G8R8A8_UNORM), colorType 4 (kBGRA_8888), target valid 1`. The actual
  gate is this Skia's `check_image_info` (GrVkGpu.cpp), which requires wrapped
  images to carry **both** `VK_IMAGE_USAGE_TRANSFER_SRC_BIT` and
  `VK_IMAGE_USAGE_TRANSFER_DST_BIT`. My swapchain requested only
  COLOR_ATTACHMENT|TRANSFER_DST; sloppy-ui's includes TRANSFER_SRC. Adding
  `TRANSFER_SRC` fixed the wrap, and the fullscreen run then presented 277
  frames and exited 0 — the whole GLFW→vk-bootstrap→Skia→WSI path works. The
  chosen-format↔colorType mapping was also made SRGB-safe and a diagnostic
  dump of the offered surface formats stays on the failure path.
- **Layer model implemented the same day** (user requirement, recorded in
  PLAN.md "Output layer model"): `LayerStack`/`RenderLayer` with z-ordered
  layers and enable bits; `TestScene` rebuilt as five layers —
  background/image/words/aux-placeholder/clock — drawn straight into the
  target surface, audio excluded as a playback-only source. Aux starts off.
  `--check-layers` proves on the CPU: all-off is pure black (skipped layers
  dispatch nothing), each enabled layer paints only inside its declared
  region, and re-toggling returns byte-identical frames. Default snapshot is
  pixel-identical in composition to the pre-layer scene.
- **Accepted on screen later the same day** (screen unlocked, DP-4 back):
  three more bugs stood between "presenting" and "correct on the panel",
  all found with the user watching the live window:
  - The scene *shrank every frame*. The swapchain surface hands out the
    same `SkCanvas` each frame and Skia keeps its matrix, so the
    per-frame `translate`+`scale` of the output rule compounded
    (×0.9, ×0.81, …) until the scene collapsed into a dot — which is
    also why screenshots read as "solid black". A CPU snapshot renders
    one frame per process and could never show it. Fix:
    `canvas->resetMatrix()` at the top of `TestScene::draw()`.
  - A one-shot `surface->readPixels()` probe at frame 30 (kept in
    `runRenderTest`) was the decisive instrument: pre-fix it showed 15%
    non-black pixels, post-fix 100% — Skia had been writing correctly
    into the wrapped images all along.
  - Fullscreen never got the panel extent: GLFW received no configure
     while the surface stayed unconfigured, so the swapchain lived at the
     1280x720 fallback and KWin upscaled (soft text). Fix: map the window
     *before* `glfwSetWindowMonitor`, pump events until the framebuffer
     matches the monitor mode (≤600 ms), and poll
     `Swapchain::extentOutOfDate()` every 30 frames so a late configure
     still recreates the swapchain at the true extent.
  - Cosmetic: the status line's `U+2714` is missing from Liberation Sans
    and showed as tofu in the text-blob path; the prefix is now plain
    "live:".
  Final acceptance: `--render-test --monitor 1 --seconds 20` created the
  swapchain at native **1920x1200**, presented **1188 frames (~59.4 fps,
  exit 0)**, readback 90% non-black (the authored 1920x1080 canvas
  letterboxed in the 16:10 panel), and the user confirmed the scene on
  the second display (`/tmp/opencode/native2-dp.png`).

## Task 1.2 log (2026-10-23)

- Carved the seam before the socket, per PLAN.md graduation:
  `OutputSession` (session/) took the whole task 1.1 loop — resize
  handling, the periodic `extentOutOfDate()` recreate, the FIFO present
  path and the readback probe (now `Options::diagnostics`) — verbatim in
  behaviour. Cross-thread design: layer toggles land in a mutex-guarded
  *desired mask* with an incremented revision; the render thread applies
  the mask at the top of each frame and publishes the applied state, so
  the ack's revision tells the frontend which status confirms the change
  and no render-owned object is ever touched off-thread. A generic
  command queue was deliberately not built yet (YAGNI; M2 scenes will
  want one with ordering).
- Wire (`openshow::protocol`): u32-LE length + JSON, cap 1 MiB; `Decoder`
  poisons itself (and the connection) on oversize/zero-length/malformed
  frames; JSON-RPC 2.0 helpers; `hello` negotiates major=0 and answers
  with capabilities `output.status.v1`, `output.layers.v1`,
  `shutdown.v1`. Methods served: hello, ping, output.status,
  layer.setEnabled, shutdown. Everything before hello gets
  `kNotHandshaked`; unknown names/methods get structured errors.
- `RpcServer` (worker/rpc): own thread, `poll()` with 50 ms ticks so
  `stop()`/`requestStop()` land promptly; `MSG_NOSIGNAL` writes; one
  client; after `serve()` returns the worker requests its own stop —
  "lives only while connected" is implemented in the accept loop.
- UI (`openshow::ui::WorkerController`): worker path via
  `OPENSHOW_WORKER_PATH` compile definition; socket path under
  XDG_RUNTIME_DIR with the UI pid; `OPENSHOW_WORKER_EXTRA_ARGS` dev hook
  (used to put the output on DP-4 for screenshots); QML shows state,
  device, frames, revision and the five layer checkboxes wired to
  `layer.setEnabled`.
- Traps hit and fixed along the way:
  - Boost.JSON object iteration yields `key_value_pair` with `.value()`,
    not `.second`.
  - `sk_sp`/`unique_ptr` members of forward-declared types blew up in
    `main.cpp` (inline destructor paths); `OutputSession.h` just includes
    the real headers — the worker's internals are all in-tree anyway.
  - QProcess default channel mode is *closed* — the worker's log vanished
    into nowhere until `ForwardedChannels` was set.
  - The UI's first `connectToServer` fired before the worker had bound
    (the ctest client had a retry loop, the UI did not) — caught **only**
    by the on-screen check, not by any test: status sat red at "starting
    worker...". Now a 100 ms retry timer, ten seconds of patience. The
    lesson matches M0's: runtime verification finds what unit tests
    structurally cannot.
  - Both binaries now handle SIGTERM/SIGINT: the worker sets an atomic
    the frame loops check (`run(seconds, &abort)`), the UI's polls a timer
    into `quit()` so `aboutToQuit` still sends the graceful shutdown.
  - CMake 4's ctest preset key for filtering is `filter`, not `tests`
    (v6 docs vs loader); the new display-labelled test is excluded from
    the headless preset.
  - Verification pitfall: `pgrep -f "openshow-worker --socket"` matches
    the shell quoting the pattern — count real processes by binary path
    (`pgrep -af "build/src/worker/openshow-worker"`).
- Acceptance evidence: ctest `worker_rpc_lifecycle` (spawn → gated
  commands → hello → live toggle observed through status revision →
  structured errors → served shutdown → clean disconnect) passes;
  screenshots `/tmp/opencode/e2e2-edp.png` (UI: green connected, live
  counters, toggles), `/tmp/opencode/e2e2-dp.png` (scene on DP-4 driven
  by the spawned worker), `/tmp/opencode/toggle-off-dp.png` (socket
  commands visibly removed and restored the images/words layers);
  worker logs show `shut down after 1756 frames`, exit 0, socket unlinked.

## Task 1.3 log (2026-10-23)

- Graduation carried out as planned: placement logic left the CLI.
  `OutputWindow::open()`'s inline monitor block became
  `setMonitor(index, settleMs)` — same map-then-fullscreen + configure-pump
  trick, now reusable while live; windowed restore returns to the open()
  size at (100,100). `OutputSession` owns the semantics: a desired/applied
  monitor pair applied at the top of `frame()` (GLFW is only ever touched
  from the render thread), swapchain destroy/recreate right there, a
  `DisplayInfo` snapshot (index/name/width/height/refreshHz) refreshed at
  open, after every move and every 60 frames, and `Options::
  kAutoSecondDisplay` implementing the FreeShow autoPosition rule (socket
  mode with no `--monitor` lands on panel 1 when one exists; `--render-test`
  stays windowed unless told otherwise).
- RPC: `output.displays` (bare array result), `output.select
  {monitor:-1|N}`, `monitor`+`monitorName` in status. `output.select`
  validates against the snapshot on the RPC thread; a missing or
  non-numeric `monitor` is invalid-params via a -2 sentinel (an early
  version defaulted it to windowed — caught in review, not by tests).
  `applyPendingDisplayChange` drops a vanished index rather than retrying
  a doomed selection every frame.
- UI: Destination `ComboBox` (Windowed + "name — WxH@RHz"), currentIndex
  bound to the worker's *applied* `currentMonitor`, activation sends
  `output.select`; the status line names the current panel.
- Traps hit and fixed:
  - **Array results were being dropped**: `handleReply`'s
    `result->is_object()` guard silently discarded the bare-array
    `output.displays` answer; the empty list then made the QML status-line
    binding throw on `displays[currentMonitor].name`, which froze *that
    text* at its defaults while the layer checkboxes (a different binding)
    kept updating — the screenshot showing "frame 0 · windowed" next to
    correctly-checked layers was the tell. Fixed both: array results
    handled before the object guard, and the QML binding made total
    (guarded panel lookup).
  - **The ctest label filter had never worked**: presets want
    `filter.exclude.label` (regex), not `labelExclude`; the wrong inner
    key is accepted and ignored, and the "headless 6/6" at 1.2's close was
    a stale build dir where the test wasn't registered yet. Two layers of
    false green in one line — `--show-only` after the fix now shows 6
    excluding a registered 7.
  - `sk_sp` lesson repeated: the frame's resize check double-recreated the
    swapchain right after `applyPendingDisplayChange` had already rebuilt
    it (setMonitor's `fResized`); the apply now clears the flag.
- Acceptance evidence: ctest scenario extended (enumerate, refuse 99 and
  missing param, arrive at monitor 0, arrive back windowed — all through
  status/revision); `/tmp/opencode/switch-sheet.png` (four states seen:
  DP-4 → eDP-1 → windowed → DP-4, socket-driven, worker exited 0);
  `/tmp/opencode/picker3-edp.png` (UI green, live "frame 222 · DP-4",
  picker on DP-4). Interactive drag-resize stays a manual check (Wayland
  cannot script it; the recreate path is the proven configure one).

## Task 1.4 log (2026-10-23)

- The user's constraint shaped everything: every pixel is rendered by
  Skia *inside the worker*, the JSON API carries asset references only,
  and the shm preview is the single sanctioned pixel flow. A preview
  therefore draws the same `TestScene::draw()` the swapchain gets, into
  a 640x360 `SkSurfaces::RenderTarget` (RGBA8888, `Budgeted::kNo`) on
  the output's existing `GrDirectContext`; Ganesh's `readPixels` (the
  no-context overload) converts and lands RGBA8888 straight into the
  claimed shm slot. The pane shows the audience backend, not a CPU twin.
- Transport: one POSIX shm segment per stream
  (`/openshow-preview-<pid>-<generation>`), three slots, 64-byte
  segment header + per-slot 64-byte seqlock header + payload
  (`protocol/preview_wire.h`, magic `OVSP`, layout v1). The session
  picks the generation at `preview.start` so the reply is truthful
  before anything exists; the **render thread creates** segment and GPU
  surface on the next frame, so the consumer maps on the first
  `preview.ready`. `PreviewStream` lives behind a `std::shared_ptr`
  held by session and RPC alike: a `preview.released` landing during
  teardown can never touch a retired producer, and the last owner
  unlinks the name.
- Slot ownership is a seqlock (odd = mid-write, even = stable); the
  consumer re-reads the sequence around its copy and discards on
  change. The producer claims only slots whose last publish was
  released — with all three owned the frame is dropped and counted.
  The output loop never waits on a preview consumer.
- Control: `preview.start {width?,height?,fps?}` (defaults 640x360 at
  15 fps) / `preview.stop`; worker→UI `preview.ready
  {slot,seq,width,height,rowBytes,revision,publishedNs}`; UI→worker
  `preview.released {slot,seq}`; capability `preview.shm.v1`; status
  gained `preview {running,name,width,height,fps,published,dropped,
  error}`. `RpcServer::flushNotifications` drains ready events on every
  50 ms poll tick, timeouts included, so notification latency is one
  tick at most.
- Traps hit and fixed:
  - **Boost.JSON keeps small integers in its int64 kind.** Params
    guarded with `is_uint64()` silently rejected every number the UI or
    the Python driver sent — `preview.start` answered with defaults, no
    error. Fixed with a `jsonUint()` accepting both kinds; ctest D
    asserts an explicit 320x240 echo so this cannot regress quietly.
  - **The first `preview.ready` must be released even when its frame is
    never copied.** It doubles as the "segment exists" signal; holding
    it owns that slot forever. ctest D failed exactly this way (only two
    slots ever rotated; drops grew because a slot the test had never
    read was stuck unreleased). The test now releases it right after
    mapping; the UI consumer already releases every notification it
    saw, which is why the live UI never showed the stall.
  - **`preview.start` would lie unless mapping is deferred**: the reply
    arrives one render frame before the segment exists. The contract is
    documented in preview_wire.h — name in the reply, map on first
    ready.
  - **`PreviewView::paint()` used `drawImage(QPoint, image)`**, which
    paints at the image's native size: a 640-wide frame overflowed the
    480-wide pane (photo flush-left, title clipped, 640/480 ≈ 1.33 zoom
    anchored top-left). Fixed with an explicit aspect-fit `QRectF`
    destination. The raw frame dumped from the slot
    (`/tmp/opencode/preview-frame.png`) proved the wire was never at
    fault — the defect lived entirely in the QML item.
  - Smaller ones: `shm_open` always takes a mode argument (two-arg
    calls do not compile); `pgrep/pkill -f` self-matches the invoking
    shell — bracket the pattern (`openshow-[uw]`); raising windows for
    screenshots under a live KDE session needs the KWin Scripting D-Bus
    (`org.kde.kwin.Scripting.loadScript`, with an exact-caption match —
    Konsole's title also started with "OpenShowQt").
- Acceptance evidence: `/tmp/opencode/preview_smoke.py` (Python
  consumer straight off the wire: mmap by notified name, seqlock-checked
  copies; a deliberately stalled consumer raised drops while the output
  presented frames 317→419; stop + shutdown exit 0);
  `/tmp/opencode/preview_dump.py` → `/tmp/opencode/preview-frame.png`
  (raw RGBA from a slot — scene complete, letterbox rule correct);
  `/tmp/opencode/preview-ui-edp.png` (UI pane live, "preview · 77
  frames shown · 0 dropped", destination on DP-4); ctest scenario D
  green inside desktop 8/8.

## Task 1.5 log (started 2026-10-23)

- Architecture reviewed with the user before coding; PLAN.md rows 1.5a–f
  record it. Two user requirements shaped it: YUV (not RGBA) upload so
  the texture units do the colour conversion and bandwidth drops ~3×;
  and redraws must be event-driven (a static slide must not repaint),
  which graduates the frame loop into a scheduler in 1.5e.
- **1.5a** DONE. `src/worker/media/` built as static `openshow::worker_media`
  so tests link it directly; public headers carry no FFmpeg or Qt types.
  - `FFmpeg.h` is now the module's single `extern "C"` include point;
    `main.cpp` was switched to it (its inline wrapper was the second copy).
  - `MediaProbe.{h,cpp}` reports per stream: codec, profile, size, pixel
    format, guessed fps + whether the container guarantees constancy,
    colour space/range, sample rate/layout/format, and decoder
    availability from `avcodec_find_decoder`. `ProbeStatus` is a
    structured error: `kOpenFailed` (missing/unparseable, message carries
    FFmpeg's reason) and `kNoUsableStreams`.
  - ctest `media_tests` runs in **both** presets and exits 77 (skip) if
    the ffmpeg CLI cannot generate fixtures. Seen: "h264 Constrained
    Baseline 320x240 yuv420p 30.00fps", "aac 44100Hz stereo (fltp)",
    garbage file → "Invalid data found when processing input", WAV →
    one usable stream, no video. Desktop **9/9**, headless **7/7**,
    `worker_is_qt_free` green with the library linked into the worker.
  - Gotchas: FFmpeg 9 renamed `FF_PROFILE_*` → `AV_PROFILE_*`
    (`AV_PROFILE_UNKNOWN`); `av_err2str` is a C99 compound literal, use
    `av_strerror` into a buffer; lavfi `sine` is **mono** by default —
    the stereo fixture needs `-ac 2`; the x264 fixture reports colour
    space "unknown", so the test prints colour info rather than
    asserting it (the 1.5f real-world fixtures carry BT.709 and the
    probe will be exercised on them).
- **1.5b** DONE. `FrameRing.{h,cpp}` and `VideoFrameSource.{h,cpp}`.
  - `FrameRing` is the bounded I420 queue: `claim → publish` (producer),
    `poll → release` (consumer), PreviewStream's "claimable only once
    released" rule, never grows. The producer **waits** on a full ring
    (a dropped decode frame would falsify the benchmark) but every wait
    honours `stop()`. `stop()` drains visible frames then fails fast —
    that is the consumer's "decode finished" signal. pimpl keeps FFmpeg
    (`av_image_alloc`) in the .cpp; `VideoFrame`/`Stats` are plain types.
  - `VideoFrameSource` owns container + decoder + one decode thread. It
    probes first (reusing MediaProbe), picks the largest decodable video
    stream, decodes to the ring at the coded size, and only builds a
    swscale context when the stream is **not** yuv420p (I420 in →
    `av_image_copy`, else convert). pts comes from `best_effort_timestamp`
    scaled by the stream time base, falling back to a synthetic clock.
    `loop=true` seeks to 0 and flushes; ring sequence numbers stay
    monotone across the wrap while pts wraps — exactly what 1.5e's
    clocked pick loop needs. `requestStop` + `join` are idempotent.
  - ctest `media_tests` gained four scenarios, all green desktop **9/9**
    and headless **7/7** (worker still Qt-free): ring contract (FIFO
    order, monotone sequence, abort invisible, stop-drains-then-fails,
    stats); 1080p25×4s decodes to exactly **100 frames in 0.29s =
    ~350 fps** (≫2× real-time, acceptance ≥50fps), peakFilled 1 ≤ 6;
    backpressure (2-slot ring → producer stalls at produced≈consumed,
    requestStop wakes the parked producer in **0.002s**); loop (80
    frames past a ~60-frame clip, loops=1, monotone sequence, pts wraps,
    eof never set).
  - Note: the 1080p decode ran ~350 fps, so the benchmark is CPU-decode
    bound by how fast the ring is drained, not by libavcodec — the real
    1.5 number to watch is decode-to-present under the GPU path (1.5d)
    and the soak (1.5f), where pacing and A/V drift matter, not raw
    decode throughput.
- **1.5c** DONE. `AudioRing.{h,cpp}`, `AudioClock.h`, `PipeWireSink.{h,cpp}`
  and the worker dev flag `--pw-check [--seconds S]`.
  - `AudioRing` is the lock-free SPSC ring of interleaved stereo F32:
    monotone head/tail counters (no empty/full ambiguity), write capped
    at free space, read capped at available, two-memcpy wrap. Bounded at
    construction (2 s at the requested rate), never grows.
  - `PipeWireSink` owns a `pw_main_loop` thread, `pw_context` and one
    playback stream (`openshow-bench`, F32 stereo, `PW_KEY_NODE_LATENCY
    1024/48000`), connects with AUTOCONNECT|MAP_BUFFERS and waits ≤1.5 s
    on a condvar for STREAMING; failure = "daemon unavailable" reported
    through `start()` so callers skip rather than fail. The `.process`
    callback drains the ring, zero-pads the shortfall (counting
    `underrunEvents`/`underrunFrames`), sets chunk size/stride, tallies
    callback-arrival intervals into a fixed histogram (2 ms × 500
    buckets) and hands the server's stream-time report to the clock —
    no allocation, no lock in the data path.
  - `AudioClock` reads through the server's `pw_time` (`ticks` = frames
    the sink is reading, `now` = CLOCK_MONOTONIC ns, rate 1/48000),
    extrapolating between callbacks under a tiny seqlock — the clock
    ticks at server discipline, not at buffer-slice granularity.
    Delivered-frames/rate remains as fallback (and as feed accounting);
    `anchor()` clears the stream-time report. This upgrade came out of
    the live runs: with a full ring the server pulls one 12288-frame
    slice per ~256 ms, so a delivered-count clock can never resolve
    50 ms drift — the server-disciplined tick can.
  - Live evidence on this machine (speaker sink): `--pw-check --seconds
    10` → rate 48000, 39 callbacks, p99 258 ms, **0 underruns**, clock
    9.971 s vs wall 10.008 s (+37 ms total, mostly initial queue fill);
    `--seconds 60` → 235 callbacks, **0 underruns**, clock 60.059 s vs
    wall 60.039 s (**drift −20.1 ms**), exit 0. The p99 ≈ slice/rate is
    expected pacing, not a defect — the callback fires when the server
    needs the next slice. `--pw-check` verdicts on the post-warm-up
    window (underrun delta = 0, |window drift| < 50 ms) so the first
    fill does not count against "fed runs never underrun".
  - `media_tests` gained `exerciseAudio()`: ring caps/wrap-order/reset,
    unrated clock reads zero, clock = delivered/rate, anchoring restarts
    and drops the stream-time report, and stream time extrapolates from
    a reported tick. Automated tests stay **silent** — no PipeWire null
    sink exists on this machine, so the live tone run is manual
    acceptance evidence. Desktop **9/9**, headless **7/7**.
  - PipeWire 1.6 gotchas learned the hard way: `pw_stream_new` takes a
    `pw_core*` from `pw_context_connect` (that connect is where "no
    daemon" surfaces; teardown is `pw_core_disconnect`); the enums carry
    no `_t` typedefs (`enum pw_stream_state`); `spa_format_audio_raw_parse`
    returns **positive** (4) on success, negative errno on failure — a
    `== 0` check silently loses the negotiated rate, which then starves
    any consumer that sizes itself from it; there is **no**
    `pw_main_loop_lock` — PipeWire rejects cross-thread object calls
    ("called from wrong context… Operation not permitted"), so the loop
    thread itself destroys stream and core after `pw_main_loop_run`
    returns and `stop()` waits on that before destroying the context;
    without `PW_KEY_NODE_LATENCY` the server hands the whole ring per
    callback (~4 Hz) and any timing histogram measures the wrong thing.
- **1.5d** DONE. `render/YuvGpu.{h,cpp}` (`GpuYuvUploader`), the new static
  `openshow::worker_gpu` library (VulkanContext moved into it), and the
  headless test `gpu_yuv_tests`.
  - `VulkanContext` refactored: `createHeadless()` (no GLFW extensions, no
    surface) shares the device path with `create(window)`; the device is now
    built with vk-bootstrap `custom_queue_setup` carrying the graphics family
    **and** a transfer-capable family when one exists (RADV STRIX1: gfx 0 +
    compute/xfer 1, verified "uploads on queue 1"); single-family GPUs get
    uploadQueue() == queue() and callers skip cross-queue handling. Queues
    are fetched with `vkGetDeviceQueue` because custom setup leaves queue
    bookkeeping to the app.
  - `GpuYuvUploader`: fixed 4-slot ring, never grows. Each slot is a
    host-visible coherent staging buffer (Y at 0, UV at alignUp(w·h, 256))
    plus two device-local optimal-tiling images — R8 w×h and R8G8 w/2×h/2 —
    created CONCURRENT over {graphics, upload} (sharing mode is per-object;
    device queue create-infos have none). Per upload: layout barrier
    (UNDEFINED→TRANSFER_DST first use, SHADER_READ→TRANSFER_DST on reuse),
    both `vkCmdCopyBufferToImage` calls, TRANSFER_DST→SHADER_READ, submit on
    the upload queue, fence. **One `SkImages::TextureFromYUVATextures` wrap
    per slot** (GrVkImageInfo with zeroed fAlloc, shader-read layout,
    queue family IGNORED — same external-wrap pattern the swapchain uses);
    the layout never leaves shader-read after the first upload, so the same
    SkImage serves every later frame and YUV→RGB happens inside the
    compositing draw (no RTT). Consumer contract: upload() blocks briefly
    for the next slot and *refuses* when all four are held; `release(slot)`
    / `releaseAll()` return them; 1.5e upgrades release to carry the flush's
    GPU semaphore.
  - Evidence (`gpu_yuv_tests`, RADV headless — needs a GPU but no display
    server; skips 77 otherwise): 12/12 probe pixels (3 luma variants × 4
    chroma bands) matched `SkColorMatrix_YUV2RGB(kRec601_Limited)` **exactly**
    (tolerance ±3 for dithering was never needed); red/blue band structure
    verified; the 5th unreleased upload is refused and uncounted; after 8
    uploads slotsCreated is exactly 4 (reused frames rewrap nothing); upload
    cost instrumented per stage: 64×64 stage 0.24 ms / gpu 1.27 ms totals
    (per-frame fences dominate — the threaded 1.5e loop removes the wait
    from the producer). Desktop **10/10**, headless **8/8**; `--render-test`
    still presents after the VulkanContext refactor (182 frames/3 s,
    readback non-black); `worker_is_qt_free` green with the new lib linked.
  - Gotchas: vk-bootstrap's selector **requires present by default** —
    headless selection fails "no_surface_provided" until
    `require_present(false)`; Skia's `check_image_info` demands wrapped
    textures carry **both TRANSFER_SRC and TRANSFER_DST** usage bits (a
    copy-only destination returning a null wrap with no message — read the
    validator); `VkBufferImageCopy::bufferRowLength` counts **texels, not
    bytes**, so the R8G8 UV plane silently copies at double row stride (the
    R8 Y plane hides the same bug, its texel is one byte); this revision has
    no `SkImageInfo::MakeN8Premul` (use `Make(w,h,kRGBA_8888,kPremul)`) and
    `SkSurfaces::RenderTarget` lives behind `SkSurfaceGanesh.h`;
    `vkb::Instance` is not constructible from a raw `VkInstance`, so the
    built one is stored; and pimpl'd classes must out-line the **constructor**
    as well as the destructor (a `= default` ctor in the header makes GCC
    instantiate member-cleanup paths against the incomplete type).
  - Note for 1.5e: when a GPU has a single queue family, uploadQueue() ==
    queue() and `VkQueue` is **not** thread-safe — the producer must not
    submit there concurrently with Skia; same-family GPUs will need uploads
    serialized behind the render thread (or accepted in the same queue with
    a host-side lock) when the frame loop lands.
- **1.5e** DONE. `OutputSession` became the presentation scheduler; new
  `media/AudioFeeder.{h,cpp}` (`AudioFileFeeder`) and two development modes:
  `--av-bench FILE [--monitor N] [--seconds S] [--no-audio]` and
  `--idle-check` (with `--render-test`, ctest `render_idle_check`,
  `display` label).
  - **Render-thread-only Skia** (this revision documents no `GrDirectContext`
    thread-safety): every Skia call lives on the render thread. The uploader
    split expresses it — `upload()` is pure Vulkan and runs on any thread
    (returning the slot index), `imageForSlot()` wraps lazily on the render
    thread (the wrapper is the uploader's own `sk_sp`, so `acquireImage()`
    hands the session a safely borrowed raw pointer), and the session seam
    (`VideoPresenter` + `VideoSubmission`) carries only slot tokens —
    no bitmaps and no Skia types cross the thread boundary.
  - **Release protocol**: every draw that samples a video slot attaches a
    **fresh** `VkSemaphore` to its flush via
    `Swapchain::flushAndPresent(..., extraSignal)` (a 1-or-2 element
    `GrBackendSemaphores::MakeVk` array in `GrFlushInfo`; every failure path
    there runs after `flush()+submit()`, so an attached semaphore is always
    signalled and hand-off stands regardless of the return value).
    `release(slot, doneVk)` appends to the slot's `pendingDone`; the next
    upload to that slot waits on **all** of them at
    `FRAGMENT_SHADER`-read stage and destroys them behind its own fence.
    A static per-slot binary semaphore cannot express this: static-hold
    redraws of the same texture would double-signal it.
  - **Event-driven loop**: `run()` presents only when a revision/monitor/
    preview-generation changed, the framebuffer resized, the swapchain was
    recreated (`fNeedsPresent`), or a pending video slot came due against
    the clock; otherwise it sleeps in `glfwWaitEventsTimeout`, woken by
    display events or `wakeRenderThread()` — `glfwPostEmptyEvent`, the
    portable thread-safe nudge (documented deviation from PLAN's literal
    "eventfd + display fd": GLFW owns the display connection; the intent,
    one poll-able wake for commands + vsync events, holds, and it works on
    both Wayland and X11). Housekeeping (compositor extent check, display
    snapshot) runs time-based every 0.5 s and presents nothing; a swapchain
    recreate marks the loop dirty. Frame() captures the revision it renders
    for and stores it only after presenting, so commands arriving mid-frame
    re-dirty instead of being swallowed. The timed `--seconds` deadline is
    checked **before** the present branches (a due video frame every 40 ms
    otherwise re-enters frame() forever).
  - **Video path**: `setVideoPresenter/setVideoClock/submitVideo` (drop-old
    with `onDropped` for the superseded token); `frame()` draws the pending
    slot when `clock >= pts`, else statically holds the last drawn frame
    (a toggle mid-video never blanks); the `onPresented` hand-off happens at
    the **end** of frame(), after the preview publish, whose `readPixels`
    is CPU-synchronous — so by hand-off no queued GPU work samples the slot
    beyond what the done-semaphore covers.
  - **Preview content gate**: `publishPreviewFrame` republishes only when
    the presented frame count moved (`fFrames != fPreviewPublishedFrames`,
    reset when a stream is built); a cadence-throttled publish completes on
    the next allowed tick through a previewDue wake without an audience
    present. Rpc scenario D was rewritten to these semantics — every copy
    is preceded by a layer toggle (doubles as proof a command wakes the
    loop), and the stall window keeps toggling: `published` freezes, drops
    tick, releases resume, all green.
  - `AudioFileFeeder`: owns its own container handle (never share a demuxer
    across threads), `swr_alloc_set_opts2` to interleaved stereo F32 at the
    sink's negotiated rate, retry-on-zero write callback (never blocks on
    consumer state; stop cuts sleeps short), EOF drains decoder (`send_packet
    (NULL)`) then resampler, loops via `avformat_seek_file` (**six** args:
    min/ts/max/flags) + `avcodec_flush_buffers`.
  - Evidence (`--av-bench`): decode-hd.mp4 (1080p25, silent → wall clock)
    6 s: presents=150 (**25.0 fps**), missed(>25 ms)=**0**, drops=1, drift
    mean **+0.7 ms** max 11.2 ms. av-10s.mp4 (1080p25+AAC → PipeWire clock)
    12 s incl. one loop: presents=300, missed=**0**, drops=**0**, drift
    mean **+0.6 ms** max 1.2 ms, sink underruns=0 (audio-vs-wall offset
    +37 ms is the buffer latency, inside the 50 ms band). `--idle-check`
    run: "0 presents in 5.0s idle, cpu 0.9ms (0.02%), toggle wake ok →
    PASS". Desktop **11/11** (new `render_idle_check`), headless preset
    **8/8**, `worker_is_qt_free` green (AudioFeeder links no Qt). Behavior
    change to note: `--render-test` on the static test scene now presents
    1 frame instead of the old ~182/3 s — the scene has no animating
    source, so under the new rules presenting once is correct; the frozen
    diagnostics line updates with every event-driven present.
  - Bench-pipeline lessons (each cost a debug cycle, all worth keeping):
    the decode ring hands out **planar I420** while the GPU slots want
    Y + **interleaved** UV — the bench folds U/V on the upload thread
    (µs; M2 should ask swscale for NV12 output directly); upload **lookahead
    must stay under one frame interval** because the seam holds a single
    pending frame — 0.12 s lookahead made every deadline superseded before
    it arrived and nothing presented; and a **looping decoder rewinds pts**
    while the presentation clock runs on — the bench carries a loop-base
    offset (container duration) so drift means what M5 needs it to mean.
  - Accepted residual race (cosmetic, documented in code): a dirty-redraw
    or cadence-delayed publish can sample an already-released slot while
    that slot's next upload copy is in flight — never memory-unsafe (the
    copy waits on all release semaphores of prior sampling and starts only
    after the CPU-side release), worst case one blended frame. Closed by
    the hold-until-replaced slot protocol when M2 media layers land.
- **1.5f** DONE. Benchmark matrix (`--av-bench … --preview`: the 1.4 preview
  stream runs at 640x360@15 beside the output with a built-in consumer that
  pops readys and releases immediately; RADV STRIX1, Wayland session, all
  runs exit 0).
  - av-10s.mp4 (generated h264 1080p25 + AAC, PipeWire clock) 60 s:
    **1500 presents = exactly 25.0 fps**, missed=0, drops=0, drift mean
    +0.6 ms / max 1.7 ms, 6 decoder loops, preview 899 published / 0 dropped.
  - bribary.mp4 (real-world AV1 1080x1920 portrait @30 + AAC) 60 s: 1795
    presents, **missed=2 (0.111%)**, drops=1, drift mean +0.6 ms /
    **max 39.8 ms** (both outliers at a loop boundary, none mid-playback),
    0 underruns, preview 899 / 0 dropped.
  - decode-hd.mp4 (h264 1080p25, silent → wall clock) 30 s: 751 presents,
    missed=0, drift mean +0.8 ms / max 13.8 ms, preview 450 / 0 dropped.
  - **Soak** av-10s.mp4, 600 s: **15000 presents = exactly 25.000 fps for
    ten minutes, missed=0, drops=0**, drift mean +0.5 ms / max 3.0 ms,
    2344 sink callbacks with 0 underruns, 60 decoder loops, GPU slot count
    flat at 4 with 4 wraps (no leaks, steady ~0.8 ms stage + ~0.8 ms
    copy+fence per upload throughout), preview 8990 / 0 dropped.
  - **Verdict: feasible on this PC.** M5's <1% missed deadlines: worst
    observed 0.11%, and 0% in the three other runs including the soak.
    M5's <50 ms drift: worst observed 39.8 ms at an AV1 loop seam,
    steady-state max 3.0 ms. The sink's +35…40 ms audio-vs-wall offset is
    buffer latency, not slip (the video clock rides it), and M5's sync work
    can compensate it if needed. Follow-ups noted for M2: watch loop-boundary
    outliers if they ever appear mid-playback, and the accepted redraw/
    mid-copy cosmetic race (1.5e) closes with the media-layer slot protocol.
- **1.5g** DONE. Optional Vulkan validation for the hand-written code
  (user-inserted task): `VK_LAYER_KHRONOS_validation` plus a
  `VK_EXT_debug_utils` messenger, off by default, enabled with
  `--vulkan-validation` (all Vulkan modes: `--render-test`, `--av-bench`,
  `--socket`) or `OPENSHOW_VULKAN_VALIDATION=1` — the env form exists so
  `OPENSHOW_VULKAN_VALIDATION=1 ctest` covers `gpu_yuv_tests` without
  plumbing flags through it. vk-bootstrap does the work (extension,
  messenger, hard failure when the layer isn't installed, checked up
  front for a clear error); `VulkanContext` installs a custom callback
  (severity warning+error), destroys the messenger before the instance,
  and dedupes by message id — the first three occurrences in full, then
  every 60th with a seen-count, because a 25 fps loop otherwise floods.
  - **It caught a real bug** (fixed): the uploader's barriers in
    `YuvGpu.cpp` named `VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT` as source,
    destination and semaphore-wait stage inside upload-queue command
    buffers — graphics stages are not allowed on the compute+transfer
    family (VUID-vkCmdPipelineBarrier-dstStageMask-06462,
    -srcStageMask-06461, VUID-vkQueueSubmit-pWaitDstStageMask-00066).
    Semantically neutral to fix: a fragment stage on that queue could
    never have contributed ordering — the release-semaphore wait is the
    real protection, since its signal retires the whole Skia sampling
    batch. Now TOP_OF_PIPE→TRANSFER for layout-in, TRANSFER→BOTTOM_OF_PIPE
    for the shader-read transition (cross-queue visibility rides the fence
    the upload waits before publishing the slot), and the wait stages are
    BOTTOM_OF_PIPE.
  - **Skia itself validated clean**: 20 s AV1 + preview runs (599 presents
    through swapchain, wrapped external YUV textures and cross-queue
    release semaphores) produced zero messages from Skia's Vulkan usage.
    The one recurring message is loader noise on this Fedora install —
    `terminator_CreateInstance: … libvulkan_dzn.so. Skipping this driver`
    (the D3D12 ICD refusing to create on Wayland, harmless, unrelated to
    anything here).
  - Verified with the layer on (RADV STRIX1): render-test clean,
    `--idle-check` still PASS (0 presents, 0.02% CPU), av-bench av-10s +
    preview 200 presents 0 missed with no validation output, bribary 20 s
    + preview zero validation messages (its 2 missed / 1 underrun are the
    known loop-seam behaviour), gpu_yuv_tests pass headless with the env
    var, socket mode clean. Without flag or env the suites are unchanged:
    desktop **11/11**, headless **8/8**.
- Follow-up (2026-10-24, user-verified): the bench's video placement had
  been stretch-to-fill **since 1.5e** — never aspect-aware — so 16:9 clips
  on the 16:9 panel looked correctly fitted and a non-16:9 clip suddenly
  looked wrong (the user's system gained a hardware h264 decoder around
  the same time, but that is unrelated: the worker requests no VA-API
  decoder and decodes in software). `OutputSession`'s bench draw now
  fits the frame centred with its aspect preserved; the letterbox/pillar
  bars are the scene background, and real media layers will bring their
  own geometry in M2. Screenshots: `/tmp/opencode/fit-916.png` (before:
  portrait stretched full-frame) vs `/tmp/opencode/fit-916-fixed.png`
  (after: pillarboxed, undistorted), 16:9 unchanged
  (`/tmp/opencode/fit-169-fixed.png`).
- The follow-up itself needed a follow-up: the user still saw background
  "right and bottom", and was right — a **leftover canvas transform**
  was warping the video quad. `TestScene::draw` applies its output rule
  (fit the authored 1920x1080 into the swapchain: at this window, scale
  0.9 + translate) and only resets the matrix on *entry*, so anything
  drawn after it — the video quad, computed in swapchain pixels — is
  multiplied by that transform: the fitted rect (590.6, 0, 1137.4, 972)
  actually landed at (567.6, 0, 1059.7, 874.8): a ~10% gap at the bottom
  and shifted left, i.e. exactly the reported asymmetry. Proved with a
  temporary red probe frame drawn at the surface bounds: it landed
  inside the window content, not at its edges; drawing the video on the
  identity matrix (save/reset/restore) fixed it and the probe then
  hugged the window. Kept diagnostics: the window now logs
  "logical/framebuffer" sizes (this Wayland session scales 1.35:
  1280x720 logical → 1728x972 physical, which is why the swapchain size
  looked bigger than the window) and the first video draw logs its quad.
  The quad log also gained whole-pixel rounding — the old `-3.05e-05`
  top edge was float32 round-trip noise (972/1920 = 0.50625 is not a
  binary fraction; multiplied back it gives 972.00006), harmless but
  alarming-looking. **Lesson for M2: anything drawn onto the swapchain
  canvas after the scene must set up its own matrix — the scene owns
  the transform but only restores on entry.** Gates after the fix:
  desktop **11/11**, headless **8/8**.
- **1.5h** DONE (user-inserted: enable hardware decoding if available
  and log the status). VA-API decode via the hwaccel API:
  `av_hwdevice_ctx_create(AV_HWDEVICE_TYPE_VAAPI)` as the decoder's
  `hw_device_ctx`, plus a `get_format` hook picking `AV_PIX_FMT_VAAPI`
  when offered. Two discoveries worth keeping: FFmpeg's `*_vaapi` codecs
  are **encode-only** (the first design, `avcodec_find_decoder_by_name
  ("h264_vaapi")`, could therefore never match), and wrapper decoders
  ignore hwaccels — this build resolves AV1 to **libdav1d**, which
  decoded in software while silently skipping the hook; the ffmpeg CLI's
  own remedy ("Selecting decoder 'av1' because of requested hwaccel
  method vaapi") is reproduced by preferring the decoder registered
  under `avcodec_get_name(codec_id)` whenever hardware is enabled.
  VAAPI frames are copied back with `av_hwframe_transfer_data` (NV12 for
  8-bit, P010 for 10-bit) and converted to the ring's I420 by a swscale
  context built **lazily from the first frame's actual format** — which
  also removed the old open-time `decoder->pix_fmt` assumption, so a
  negotiated software fallback with a non-yuv420p stream now converts
  instead of erroring every frame. Status logs once per open
  ("openshow-worker: video decode: h264 (VA-API hardware decode)" /
  "(software decode; VA-API device unavailable: ...)"), plus a
  "converting nv12 to I420" line when conversion exists;
  `--no-hw-decode` forces the software path for A/B runs. Measured on
  the 890M: h264 and AV1 both negotiate VA-API; AV1 hardware max drift
  **1.0 ms** vs 33–53 ms outliers in software; zero decode errors.
  Known waste filed as **1.5i**: hardware frames still round-trip
  NV12→I420→UV-re-interleave into GPU slots that are themselves
  NV12-layout (R8 + R8G8) — the fast path is an NV12 ring or, best, a
  zero-copy `drm_prime` import wrapped as
  `VK_FORMAT_G8_B8R8_2PLANE_420_UNORM`. Gates: desktop **11/11**,
  headless **8/8**.
- **1.5i** DONE — device groundwork for the two-plane YUV path
  landed first: the device is created with the core
  `samplerYcbcrConversion` feature and mirrors it into Skia through the
  `features2` pNext chain. That node is load-bearing — Skia reads 1.1
  features only from the chain (the old empty `pNext` made it drop every
  multi-planar format from its caps and never build a
  `VkSamplerYcbcrConversion`). With it enabled, the vendored Skia
  registers `G8_B8R8_2PLANE_420_UNORM` (color type kRGB_888x) and
  `G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16` (kRGBA_1010102) — the
  latter being the Vulkan name for the P010 memory layout (there is no
  `VK_FORMAT_G10_B10R10_2PLANE_...` format at all; my earlier "not in
  Skia" note had chased that phantom name).
  The design (user-directed, `get_buffer2`): FFmpeg's normal decoder is
  handed **host-visible LINEAR** two-plane Vulkan images as its output
  buffers, then each slot is copied LINEAR→OPTIMAL (same two-plane
  format, one `vkCmdCopyImage`) and the optimal image is wrapped for
  Skia via `BorrowTextureFrom` + a ycbcr sampler (`GrYUVABackendTextures`
  needs one texture **per plane**, so the single two-plane wrap must go
  through `BorrowTextureFrom`, not the YUVA-texture path). Probed on the
  890M: RADV binds host-visible+coherent memory to LINEAR two-plane NV12
  and P010 and returns per-plane offsets/rowPitch (NV12 1920→rowPitch
  2048 padded, P010 rowPitch 3840) — exactly the mapped-plane pointers +
  `linesize` a `get_buffer2` AVFrame needs. Because the pixels reach the
  image through a CPU pointer write (software decode, or
  `av_hwframe_transfer_data`'s NV12/P010 write for the VA-API path), the
  scheme needs **no** `VK_KHR_video_decode_*`: the GPU video codec never
  writes the Vulkan image, and the slots are identical whether or not the
  hardware decoder is active. (A first pass added `VK_KHR_video_queue`/
  `video_decode_*` + a dedicated decode queue believing FFmpeg's vulkan
  hwaccel would be the producer — that is a different, true-zero-copy
  path this task does not use, so that plumbing is removed here; noted
  for the record: RADV 26.2.5 exposes Vulkan video decode for **AV1 +
  VP9 only**, no h264/hevc.) Gates: desktop **11/11**, headless **8/8**;
  `--av-bench` clean under validation.
- **1.5i implementation** (this commit): the raster-slot pipeline shipped
  as designed, with the ring and the uploader unified through one small
  contract. `media/FrameRing.h` gains `FrameFormat{kNV12,kP010}` and an
  abstract `FrameBacking` (`slotPlanes()`/`commit()`):
  `FrameRing::create(depth, w, h, format, backing)` drives `slotPlanes()`
  for every slot (a null backing keeps the old malloc'd NV12 storage, so
  the media library still carries no Vulkan/FFmpeg types and media_tests
  stay GPU-free), and `publish()` calls `commit()` **outside the ring
  lock** before making the frame visible — visible therefore means
  GPU-ready — returning false and aborting the claim when the commit
  fails. `VideoFrameSource::open()` no longer creates the ring (split
  into `open()` probe/decoder + `createRing(depth, backing)`); the ring
  format is derived at open from `av_pix_fmt_desc_get(...)->comp[0].depth
  >= 10`, and the ring is sized to the **coded** frame so a VAAPI
  transfer's `dst <= frames_ctx` dimension rule always holds.
  `GpuYuvUploader` implements the backing: slot *i* is a host-visible
  coherent **LINEAR** two-plane image mapped once for its life (plane
  pointers + driver `rowPitch` handed out as FFmpeg `data[]`/`linesize[]`
  under the semi-planar convention `data[2] = data[1] + 1`) plus a
  device-local **OPTIMAL** twin; `commit()` records one `vkCmdCopyImage`
  with two plane regions (COLOR-aspect barriers pin the LINEAR image to
  TRANSFER_SRC once and ping-pong OPTIMAL SHADER_READ↔TRANSFER_DST),
  submits it on the upload queue behind the slot's collected draw
  semaphores and waits its fence, so the producer thread blocks only on
  the GPU copy (~0.7 ms) and never on the render thread. `imageForSlot()`
  wraps each slot exactly once via `SkImages::BorrowTextureFrom` with
  `kBorrow` ownership — Skia never destroys these images — carrying a
  `GrVkImageInfo.fYcbcrConversionInfo` whose chroma siting (MIDPOINT,
  cosited fallback), chroma filter and 601/709 × limited/full model come
  from `vkGetPhysicalDeviceFormatProperties`, so the YUV→RGB conversion
  runs inside the compositing draw's sampler. The aliased hardware path
  is the quiet trick: with `transfer->buf[0]` set to a kept 1-byte
  `av_buffer_ref` ticket, `av_hwframe_transfer_data` writes **through the
  caller's** `data[]`/`linesize[]` — the VA-API NV12/P010 readback lands
  directly in the mapped slot, no intermediate frame at all.
- The bench upload thread became pure pacing (poll → pts-continuity
  lookahead sleep → `recordPts` + `submitVideo(token = slot)` →
  `ring().release(slot)` **immediately**: the producer may rewrite the
  LINEAR twin while the OPTIMAL image waits for its draw because the
  next commit gates on that draw's `doneVk`), and teardown gained
  `requestShutdown()`, which releases producers parked in a commit wait
  so a decode thread can never outlive the session. A publish failure
  while `ring.stopped()` is deliberately **not** a decode error — that
  abort is the producer unwinding, and without the distinction
  `decode-errors` was 0-or-1 per run purely on stop timing; now it is
  deterministically 0.
- Verification: gpu_yuv_tests renamed its fixture NV12 and gained a
  `FrameBacking`-contract section — write through `slotPlanes()` exactly
  as the decode thread does, `commit()`, read back through the wrapped
  image — pixel-comparing against the Rec.601-limited matrix at **delta
  0** on RADV (as do the upload-path variants; overflow-refusal and
  one-wrap-per-slot invariants still hold). media_tests gained a
  FakeBacking test: `create()` pulls every slot's planes, `publish()`
  commits the slot it reveals, and a refused commit leaves nothing
  visible with the slot claimable again. Acceptance runs (890M):
  bribary AV1 hw `--seconds 5 --vulkan-validation` → missed=0,
  decode-errors=0, copy 0.685 ms, zero validation messages, and the old
  "converting nv12 to I420" log is gone; 25 s looping run 1/749 missed
  (the historical noise band, cf. the 2/1795 soak note above); h264
  av-10s and `--no-hw-decode` (yuv420p→NV12 interleave fast path) clean.
  10-bit proof needed generated fixtures (all repo fixtures are 8-bit):
  HEVC Main10 exercises the **aliased VA-API P010** transfer — the
  "converting" log never appears — while h264 High10 fails VA-API init
  on this driver (pre-existing: the driver lacks that hwaccel) and so
  validates the sws fallback into a PACK16 slot instead; both 0 errors,
  validation clean, sampler wrapped as kRGBA_1010102. Gates after:
  desktop **11/11**, headless **8/8**.

## Direct decode memory regression (2026-10-24)

The direct software path in local commit `6809786` passed correctness and
synchronization checks but regressed deadline pacing on
`media-fixtures/av-10s.mp4` (H.264, 1920x1080, 25 fps, B frames).

The decoder allocator selected the first HOST_VISIBLE | HOST_COHERENT memory
type, which is **uncached** on this RADV device: type 2, flags `0x6`. Software
codecs repeatedly read their mapped reference pictures, unlike sequential
staging writes, so this is a poor decoder allocation even though it is valid
Vulkan memory. Cached/coherent type 5 (`0xe`) is compatible with the same
linear images. Pool exhaustion was not the cause: eight decoder images were
allocated out of the 64-image cap, and allocation never waits for a pool slot.

The fix prefers HOST_CACHED in addition to the required HOST_VISIBLE and
HOST_COHERENT flags **for decoder images only**. It falls back to the original
required flags when cached memory is incompatible. Compatibility staging,
queue depth, codec threads, copy fences, semaphores, and AVBufferRef lifetimes
are unchanged. No CPU frame copy or extra GPU copy was introduced.
`cached-decoder-images` now reports the allocation choice alongside
`decoder-images`. The GPU test queries the image's compatible memory types
independently and verifies cached coherent memory is preferred when available.

Repeated six-second runs, without competing GPU tests:

```sh
./build/src/worker/openshow-worker --av-bench media-fixtures/av-10s.mp4 \
  --seconds 6 --no-audio
```

| Allocation | Late frames per run (>25 ms) | Mean drift | Maximum drift |
| --- | --- | --- | --- |
| Original, initial three runs | 22, 14, 1 | +1.7 to +8.1 ms | 61.2 to 75.0 ms |
| Cached, initial four runs | 0, 0, 0, 0 | +0.8 to +0.9 ms | 13.4 to 17.6 ms |
| Original, interleaved A/B runs | 5, 3 | +1.8 to +2.5 ms | 65.1 to 70.5 ms |
| Cached, interleaved A/B runs | 0, 0 | +0.7 to +0.8 ms | 14.9 to 17.7 ms |

The saved original executable (`/tmp/openshow-worker-before-cache`) was used
between corrected runs. The corrected benchmark reports all eight decoder
images cached, with zero downloads, CPU frame copies, conversions, or decode
errors. GPU copy/fence cost remains around 0.6–1.2 ms per commit.
A 20-second looping run also had **0/501** late presentations, mean drift
+0.8 ms, maximum 13.6 ms, and two decoder loops. A six-second audio-clock run
(`--allow-video-copy`, still direct) had **0/150** late presentations, mean
+0.9 ms, maximum 1.8 ms, and zero audio underruns. Both retained eight cached
decoder images and zero downloads, CPU copies, conversions, or decode errors.

Validation: desktop **12/12** on RADV and headless **9/9** on software Vulkan,
with `OPENSHOW_VULKAN_VALIDATION=1 VK_LAYER_VALIDATE_SYNC=1`. The corrected RADV
API capture (`/tmp/openshow-cached-decoder.gfxr`) confirms eight decoder
allocations using memory type 5 (`0xe`), 24 frames / 24 three-plane image copies,
zero buffer-to-image uploads and zero blits. Image-to-buffer copies in this
capture are test-only pixel readbacks.

This memory-selection fix follows `6809786`; no push was performed as part
of this work.

## Task 2.1 log (2026-10-24)

- **2.1** DONE — M2 opens with the document model:
  `src/domain/include/openshow/domain/documents.h` + `documents.cpp`,
  namespace `openshow::domain`. Plain data (std only — the Qt-free/
  GPU-free boundary holds) plus every edit as a named operation on one
  `Documents` registry, because persistence (2.2) and undo (2.3) will
  wrap exactly these calls. Design decisions worth recording:
  - **Vocabulary** follows `docs/reference.md`'s mapping table: show =
    deck of slides; group = a chorus/verse **label carried by slides**
    (not a container — a group exists exactly while some slide wears
    it, so `renameGroup` cascades and `dissolveGroup` ungroups);
    playlist = FreeShow's "project" = ordered show references with
    optional section markers and repeat show entries allowed.
  - **Identity**: store-issued string ids (`"show1"`, `"slide2"`, …)
    from one store-wide counter — unique across kinds, never recycled,
    deterministic for tests. Slides live in their show's vector
    (presentation order **is** vector order) but are addressed by id.
  - **Failure contract** matches the rest of the project: `bool` +
    optional error string; a rejected operation mutates nothing (a
    dedicated test proves five different refusals leave the model
    byte-identical). Names are stored trimmed — `"Chorus "` would
    silently fork a group otherwise.
  - **Styles** are named appearance records (`#rgb`/`#rrggbb` + finite
    font size, 0 = renderer default) with a two-level resolution:
    per-slide override → show default → none. Removing a style clears
    every reference (show default and slide overrides) rather than
    leaving dangling ids. Scene compilation (2.4) consumes them.
  - **Cascade**: deleting a show reaps playlist entries naming it (a
    playlist that could never run a slide is worse than a hole);
    deleting a playlist keeps shows.
  - `domain_tests` covers all five areas with success paths, failure
    paths, ordering invariants (first-appearance groups, move
    semantics where destination names the final list), and the
    no-trace-on-refusal check. Gates: desktop **12/12**, headless
    **9/9**.
  - Test-writing traps hit (both my test's bugs, the model was right
    first pass): group expectations must match the setup they just
    performed, and "refusal leaves no trace" needs genuinely invalid
    arguments — a valid slide id in `setSlideLines` simply succeeds.

## Task 2.2 log (2026-10-24)

- **2.2** DONE — the model survives disk. Two pieces:
  - **Assets join the model** (`documents.h/cpp`): `Asset{id, name,
    sourcePath}` in a store-level registry + `Slide.backgroundAssetId`,
    with `addAsset`/`removeAsset`/`setSlideBackground`/`findAsset`/
    `assetIds`. Deliberately fs-free: `addAsset` only records the
    path (existence fails at save, naming the asset), `removeAsset`
    clears every slide's background reference exactly like styles.
    Names trimmed, paths never trimmed (spaces are legal in paths).
  - **`domain/storage.h/cpp`** — `saveService`/`loadService` over a
    directory package: `service.json` (format 1, camelCase, ids
    verbatim, `nextOrdinal`) plus `assets/<asset id>-<sanitised
    name>`. Boost.JSON is the project's one JSON library (same choice
    as `openshow::protocol`, linked PUBLIC into the domain); the
    domain stays free of Qt/FFmpeg/Vulkan. Ordering gives crash
    safety: asset bytes are copied **before** the manifest, which is
    written tmp+rename, so a `service.json` that exists always names
    files that exist.
  - **Load is a validator, not a parser**: unsupported format,
    malformed JSON, empty/duplicate ids (one id space across kinds),
    dangling style/asset/show references, and `nextOrdinal` not past
    the highest id in use are each refused with a message naming the
    cause — and the caller's `Documents` is untouched (built into a
    temporary, swapped only on success). Ids are restored verbatim
    through `friend class ServiceCodec` (defined in storage.cpp; the
    public save/load are delegates), so ids keep never recycling
    across a save/reopen boundary.
  - **Byte-stable re-saves**: the manifest records only
    package-relative paths, never the loaded absolute `sourcePath`,
    so `save(load(save(s)))` is byte-identical and re-saving skips
    copies whose source already *is* the packaged file
    (`equivalent()` short-circuit).
  - `domain_tests` `exercisePersistence()`: sample-service round-trip
    (field-by-field, incl. groups, style resolution, playlist order),
    asset copied with its bytes and reloaded pointing into the
    package, id continuity after load, byte-stable re-save, missing-
    source save failure, and six load refusals each verifying the
    target survives. Gates: desktop **12/12**, headless **9/9**.
  - Traps hit: `std::filesystem::error_code` does not exist (it is
    `std::error_code`), `boost::json::parse` takes a
    `boost::system::error_code`, `Id` is a plain `std::string`
    (no `.string()`), ofstream won't create parent directories — and
    the two stability FAILs were my test editing the store (creating
    "Later") *before* re-saving it, not a codec bug.

## Task 2.3 log (2026-10-24)

- **2.3** DONE — edits reverse, interruptions recover. Two new pieces
  in the Qt-free domain:
  - **`domain/history.h/cpp` — `History`**: bounded (128) whole-state
    snapshot undo over the observed `Documents`. Whole-state was a
    deliberate choice over hand-written inverse operations: the model
    is plain value-semantic text data, snapshots cannot drift out of
    sync, and every domain operation wraps with one
    `history.apply([&] { return docs.op(...); })`. The edit callback's
    bool *is* the operation's bool — a refused edit (2.1 contract:
    false, model untouched) is invisible to both stacks; success
    pushes the pre-edit state and clears redo (branching discards the
    abandoned branch). `undo()`/`redo()` fail with a message when the
    stack is empty.
  - **Id watermark**: snapshot restore rolls the store's id counter
    back, which would reissue an undone creation's id. History tracks
    the highest counter it has ever seen (bumped in apply, undo and
    redo) and raises the restored store to it, so the 2.1 promise —
    ids never recycle — survives undo; `clear()` (opening a different
    service) absorbs the current counter without lowering the
    watermark. Needs `friend class History` in Documents.
  - **`Autosave` (in `domain/storage.h/cpp`)**: a scratch service
    package — same format as 2.2, so recovery *is* the validating
    loadService and crash-ordered saving makes the package always
    loadable-or-absent. Nothing schedules itself (no timers in the
    domain): the caller ticks when it likes; `tick()` compares the
    deterministic manifest bytes against the last write and performs
    zero disk access when unchanged — proven in tests by deleting the
    manifest and asserting a no-change tick does not recreate it.
    `recover()` adopts the scratch as baseline, `clear()` removes the
    package after the operator saves for real or declines.
  - `domain_tests` gained `exerciseHistory()` (undo/redo of edits *and*
    creations with id identity across redo, refusal-invisibility,
    branch discard, whole-service rewind-to-empty and rebuild,
    bounded depth, id non-recycling across undo) and
    `exerciseAutosave()` (sample service with assets through the
    scratch package, no-op ticks touch no disk, refused edits do not
    dirty the baseline, fresh-session recovery matches field-by-field,
    crash-debris scratch refused with target alive, `clear`).
    Gates: desktop **12/12**, headless **9/9**.
  - Trap hit: one autosave check sequence deleted the manifest for
    the no-op assertion and then tried to recover from it — the load
    correctly failed; test reordered so recovery starts from a ticked
    state.

## Task 2.4 log (2026-10-24) — 2.4a done, 2.4b/c remaining

- **2.4 split** (PLAN rows): 2.4a shared compiled-scene type +
  compiler, 2.4b worker scene store (`scene.prepare`/`scene.present`
  with acknowledged revisions), 2.4c Qt adapters (WorkerController +
  demo fixture).
- **2.4a** DONE — new Qt-free library `openshow::scene`
  (`src/scene/`, links `openshow::domain` + Boost.JSON):
  - **`Scene`** is the frontend↔worker boundary type PLAN's ownership
    section calls for: provenance ids, a `Background` (fill colour +
    optional image asset resolved to `assetId` + path) and `TextItem`s
    whose colours (packed `0xRRGGBB`), font family and size are the
    *result* of resolving the style chain — the renderer asks nothing.
    `clearedScene()` is the black/empty restart state PLAN's recovery
    rule wants.
  - **`compileSlide(docs, show, slide, &out)`** collapses slide
    override → show default → built-in white-on-black via the model's
    own `styleForSlide`; `#rgb` short forms expand; a background
    asset's id+sourcePath are baked in; unknown show/slide fails with
    the target untouched. File existence stays the renderer's
    question (mirrors 2.2: save checks bytes, the model doesn't).
  - **Wire codec**: `toJson`/`fromJson` ("scene": 1, fixed key order
    ⇒ deterministic serialisation, so equal scenes produce byte-equal
    payloads). `fromJson` is the worker's trust boundary — wrong
    format, wrong types, colours beyond 24 bits, non-finite/negative
    font sizes are each refused naming the cause, target intact.
  - `scene_tests` (new ctest, desktop 13/13, headless 10/10):
    resolution branches, provenance, blank-stanza preservation,
    "compiled scenes don't follow later edits", byte-stable round-trip,
    six malformed-payload refusals, cleared-scene round trip.
  - Boost.JSON trap for the *tests*: `json::array items =
    obj["items"].as_array();` **copies**; tampering the payload needed
    `auto&`. (The library itself only reads through references.)
- **2.4b** DONE — worker scene store with acknowledged revisions:
  - **`OutputSession`** gains a prepared-scene store keyed by a
    monotonic revision (0 never handed out): `prepareScene` stores a
    validated scene under the next revision (no output revision moves —
    preparing renders nothing), bounded at `kMaxPreparedScenes` = 16,
    evicting the oldest revision that is neither presented nor pending;
    `presentScene` names a prepared revision as desired, and the render
    thread applies it in the new `applyPendingSceneChange()` exactly
    like the monitor/layer seams — bumping the output revision the ack
    reports, then publishing `fStatus.scene.active`. Presenting the
    already-desired revision is a no-op success; unknown/0 is refused.
  - **RPC**: `scene.prepare` (validates the payload with
    `scene::fromJson` and refuses malformed scenes with
    `kInvalidParams`), `scene.present` (names a revision).
    `output.status` now carries a `scene: {active, pending, prepared}`
    object. Protocol minor 1→2, new `scene.store.v1` capability.
  - `worker_rpc_lifecycle` grew **scenario E**: refuse an out-of-range
    colour and a future-format scene, refuse presenting an unprepared
    revision, prepare two (distinct non-zero revisions), status counts
    them, present each watching `scene.active` settle to the named
    revision at/past the ack, refuse a never-prepared revision, clean
    exit. Worker stays Qt-free (scene lib is too). desktop **13/13**,
    headless **10/10**.
- **2.4c** DONE — Qt adapters and the demo fixture; M2 closes here:
  - **`src/scene/demo.h/cpp`** — the 1.x picture as *data*: `demoDocuments()`
    builds one show (title card with a 110px `Title` override over the
    72px `Lyric` show default on the `#14161f` backdrop echoing
    TestScene's colours, a stanza keeping its blank middle line, a
    subtitle cue, a black interlude) and `compileDemoSlide` runs the
    ordinary compiler, so the demo exercises exactly the production
    path. `scene_tests` proves all four slides compile to distinct
    scenes and the title survives the wire intact.
  - **`WorkerController`**: `prepareScene(json)` sends the scene object
    as params (the worker validates); `presentScene(revision)`; and
    `presentDemoSlide(index)` compiles + prepares with an
    `fAutoPresentScene` flag — `handleReply` detects the prepare
    ack (payload carries `revision` but no `pending`/`frames`) and
    chains the present, since the revision is the worker's to hand
    out. `output.status`'s `scene` object lands in new
    `sceneActive/Pending/Prepared` properties (NOTIFY `sceneChanged`);
    Main.qml grew a scene row (button per fixture slide + store label)
    mirroring the layer Repeater.
  - **End-to-end acceptance**: scenario E's positive prepares are now
    the frontend's own `compileDemoSlide` + `toJson` payloads (the
    bytes `presentDemoSlide` puts on the socket), accepted and applied
    by the worker — "frontend edits produce scenes the worker accepts
    and reports applied". rpc_ipc_tests links `openshow::scene`; qmllint
    shows only the file's pre-existing unqualified-`worker` pattern.
    Gates: desktop **13/13**, headless **10/10**.
  - **Live smoke run** (post-tag): launched the real UI + worker; the
    operator clicked all four demo scene buttons and the store label
    settled at "scene 4 · 4 prepared" with the status footer's
    revision tracking each present — the full compile → prepare →
    present → applied loop, verified by eye. See the compositor caveat
    under Blockers for the fullscreen wrinkle found during the run.

## Task 3.1 log (2026-10-24) — text layout and typography: DONE

- **Deliverable**: the audience picture becomes data. A presented Scene now
  *renders*: the background layer fills the scene colour, the words layer
  lays the scene's text out through the new production engine, and the
  demo photo stands down; with nothing presented the 1.x demo picture
  draws exactly as before (what `--check-layers` pins).
- **New module**: `src/worker/render/SceneText.{h,cpp}` in a new static
  library `openshow-worker-render` / `openshow::worker_render`
  (Layer.cpp + SceneText.cpp; PUBLIC scene/skia/FONTCONFIG/HARFBUZZ —
  libskshaper needs harfbuzz propagated). The worker executable now
  links it; `scene_render_tests` links the same library, so the fixtures
  exercise the exact code the window draws with.
- **`SceneTextEngine`**: the worker's single font setup (fontconfig
  manager, Liberation/DejaVu typefaces, FontCollection, hard-coded
  SkUnicode — all graduated out of TestScene, whose demo title now
  borrows the engine's faces). `layout()` builds one centred paragraph
  per TextItem (lines joined with `\n`, so stanza blank lines survive);
  `layoutScene()` runs the shrink-to-fit loop: lay out, scale =
  available/needed across *all* items (relative sizes survive), at most
  four passes, floor `kSceneMinFontSize`; `drawLayout()` stacks the
  paragraphs with a `0.4 × maxBase` gap, block centred vertically.
  Constants: canvas 1920×1080, margins 160/100 authored px, default
  size 96, line-height factor 1.25.
- **SkParagraph fork API discoveries** (this Skia predates the modern
  names; probed against `/home/jim/skia/modules/skparagraph/`):
  - `TextStyle::setHeight(x)` is a **multiplier** of the font size, not
    absolute px (`Run.cpp`: `runHeight = multiplier * fontSize`) — the
    first cut passed 90 px and got a 90× tall line box, which the fit
    loop then punished by crushing glyphs to the 12 px floor (visible
    as ~2 px text). Fix: `setHeight(1.25)` + `setHeightOverride(true)`
    + `setHalfLeading(true)` for symmetric line boxes.
  - Line count is `Paragraph::lineNumber()`, not `getLineCount()`.
  - `TextStyle::setFontFamilies` takes a plain `std::vector<SkString>`.
- **`SkUnicodes::ICU::Make()` shim** (the libskshaper link dependency)
  moved with `HardCodedUnicode` to the bottom of SceneText.cpp; the
  class now has external linkage inside `openshow::worker` for it.
  `SceneText.cpp` gets `-fno-rtti` (derives from RTTI-off Skia classes).
- **Live wiring**: `OutputSession::applyPendingSceneChange` copies the
  store entry into a `shared_ptr<const Scene>` under the lock and hands
  it to `TestScene::setActiveScene` (render thread only). The shared_ptr
  matters: the store can evict the revision while it is still on air.
  WordsLayer caches the layout per scene pointer, rebuilt only when a
  new scene arrives. The clock footer keeps drawing over scenes — a
  development diagnostic, and its layer toggles independently.
- **Worker CLI**: `--snapshot PATH --scene-file FILE [--width --height]`
  renders any `scene.prepare` wire JSON on the CPU through the same
  engine (JSON parse via `boost::json::parse(text, ec)`, then
  `scene::fromJson`).
- **Fixtures** (`tests/scene_render_tests.cpp`, `-fno-rtti`,
  SKIP_RETURN_CODE 77 when no fonts): cleared scene draws zero ink; demo
  interlude is one flat colour; ink centres horizontally/vertically;
  wrapping respects margins and fills ≥80 % of the wrap width; a
  12-line 260 px stanza fits with `scale < 1`, 12 layout lines and the
  snapshot contained by the margins, while a stanza that already fits
  keeps `scale == 1`; unknown family, Greek/accented/em-dash shaping
  and a CJK string all draw real ink through fallback; stacked items
  keep their size ratio; a `toJson`→`fromJson` round trip renders a
  **byte-identical** PNG. Two measurement policies earned their keep:
  background comes from a corner pixel (the PNG round trip shifts
  nominal colours through colour management), and multi-line alignment
  is judged by bounding-box centre — per-line glyph *mass* centroid
  legitimately drifts with text density (492 vs 480 on the demo title
  card).
- Gates: desktop **14/14**, headless **11/11**.

## Task 3.2 log (2026-10-24) — image/background composition, transitions: DONE

- **Deliverable**: a presented scene composes its picture and arrives like
  one. The background layer draws the scene colour and the cover-fitted
  background image; presenting another scene crossfades between the two on
  the reference's two tracks, or cuts in one frame when asked.
- **Compositing** (`SceneText.{h,cpp}`, still the layer-free primitive
  module): `drawSceneBackground()` fills the colour then draws the image
  cover-fitted — scale `max(W/iw, H/ih)`, centred, overflow clipped to the
  authored rect so a portrait window's letterbox bars stay black. Cover is
  the lyric default; the model carries no fit field yet, so contain/fill/
  blur (and the blur-pixel check the reference itself defers to M4) wait
  for the UI to expose the choice. `SceneImageCache` decodes through
  SkCodec into N32 premul and keeps every path it has seen — including
  failed decodes as cached nulls — so an unchanged background survives
  lyric changes without re-decoding (the M3 continuity rule) and a fade can
  hold the outgoing and incoming pictures at once. A missing file leaves
  the colour fill standing alone.
- **Transition design**: per-layer alpha dissolve, no render-to-texture
  (the layer model's rule). Background: the outgoing scene draws at full
  alpha, the incoming one over it at `mediaBlend` — the over-composite of
  two opaque pictures is an exact lerp, so solid backgrounds mix exactly
  and images crossfade. Words: old at `1 - textBlend`, new at
  `textBlend` — the classic lyric cross-dissolve. The fork's
  `Paragraph::paint` takes no paint (verified against
  `modules/skparagraph/include/Paragraph.h`), so the words dissolve wraps
  the block in one group-alpha `saveLayer` — Skia's own "transparency
  straight into the paint"; the layer is word-bounds sized and exists only
  while a fade is in flight. Layer.h's header comment now records the
  exception.
- **Two clocks**: media 800ms, text 500ms — FreeShow's per-tab transition
  defaults (docs/reference.md §7), constant-documented in TestScene.cpp.
  `SceneTransition{fFrom, fMediaBlend, fTextBlend}` lives on TestScene and
  `advanceTransition()` computes both blends once per frame from the loop's
  monotonic `timeSeconds` (the first drawn frame seeds the start, so the
  fade measures from when it is actually seen). The scene-aware layers read
  the struct by pointer — FrameContext stays scene-free per Layer.h's
  design. WordsLayer caches layouts in two slots so neither side of a fade
  re-lays-out mid-dissolve.
- **Trigger semantics**: transition belongs to the present path, not the
  Scene model. `TestScene::setActiveScene(scene, kind)`: first
  presentation (nothing on air) pops in, a cut clears any fade in flight
  and swaps in one frame, a fade keeps the outgoing scene alive as
  `shared_ptr` until the blends finish. `OutputSession::presentScene` grew
  a `SceneTransitionKind` parameter (default fade) carried to the render
  thread beside `fDesiredScene`; `scene.present` accepts an optional
  `"transition":"cut"|"fade"` — non-string or unknown names are
  `kInvalidParams` before the session is touched; no capability change,
  old clients see exactly the old behaviour.
- **Verification**:
  - `openshow-worker --check-transition` (ctest `render_scene_transition`)
    drives the *live* state machine through drawn frames at 1920×1080 and
    asserts: first-present pops; the media midpoint (+0.4s) is the
    measured corner average; words still visible there but gone at exactly
    +0.5s while media still travels (the split tracks); completion at
    +0.8s leaves no trace; a cut swaps in one frame; a cut cancels a fade
    mid-flight. `RasterFrame::render` grew a `timeSeconds` parameter
    (defaulted, the 1.x checks are untouched).
  - `scene_render_tests` adds four fixtures: cover fit both ways (wide
    image shows its centre third with image colour in the top/bottom rows —
    no letterbox; portrait image crops to a uniform middle band), missing
    file falls back to the colour fill with words intact, crossfade stages
    t=0/0.5/1 with the corner pixel measured against the endpoint average
    plus a split-track frame (media 1.0, text 0.3), and the layer toggles
    — background off leaves words on the black clear, words off leaves the
    composed image with zero ink (the pixel twin of the UI's layer panel,
    which has existed since M1 and whose dispatch skip `--check-layers`
    pins for the demo picture).
  - `worker_rpc_lifecycle` scenario E now also presents with
    `transition=cut` and `transition=fade` (acknowledged, settled) and
    rejects `"spin"` and `true`.
- Gates: desktop **15/15**, headless **12/12**.

## Task 3.3 log (2026-10-24) — minimal lyric entry and navigation: DONE

- **Deliverable**: the PLAN sentence, literally — a newly created song can
  be presented. `document_present_tests` performs it end to end: type a
  song with two slides into the same controller QML binds, compile each
  slide with the ordinary `compileSlide`, and prepare + present it on a
  live `openshow-worker`, with `output.status` confirming the revision on
  air (and again after an undo, whose rolled-back state presents cleanly).
- **`DocumentController`** (new, `src/ui`, built as the static
  `openshow::ui-core` library — Qt Core only, AUTOMOC, so the headless
  fixtures drive the very class the panel binds): owns the session's
  `Documents` with `History` (2.3) and `Autosave`, exposed to QML as the
  `docs` context property. Properties: `songs`/`slides` (plain
  QVariantMaps), `currentSongId`, `currentSlide`, `currentSlideText`,
  `dirty`, `serviceDirectory`, `canUndo/canRedo`, `status`. Operations:
  create/remove song, add/remove slide, `setSlideText`, `selectSong/
  selectSlide/stepSlide` (clamped), `currentSceneJson()/
  sceneJsonForSlide()`, `save/load`, `undo/redo`.
- **Every mutation runs through `History::apply`** (a refused edit stays
  invisible, per the 2.1 contract), so undo covers everything the panel
  does. Text editing is **debounced 600ms**: keystrokes park a pending
  value; one history entry (and one autosave candidate) lands when typing
  pauses or before any other operation runs (`flushText` is the synchronous
  edge, which is what the tests drive). A net-zero burst commits nothing.
  Undo/redo re-anchor navigation when the restored state removed the
  current song or slide — no dangling index.
- **Persistence**: service at `AppDataLocation/service`, autosave at
  `AppDataLocation/autosave` (constructor takes a scratch base for tests).
  Startup follows the 2.3 rule (autosave wins over the saved service — it
  is newer), then: an explicit save clears the autosave (nothing is
  unsaved); a discard-on-load clears it too, so thrown-away edits cannot
  resurrect at next startup. One trap found on the way: a virgin
  `Autosave::tick` always writes (its fingerprint is in-memory), so the
  constructor and save must **not** baseline-tick, or autosave would
  shadow the service forever and every start would "recover" nothing.
  A commit deliberately does not emit `slideTextChanged` — QML re-reads
  the editor field only when navigation/undo/load changes it (the panel
  re-establishes the binding with `Qt.binding`, not assignment).
- **Presentation hand-off**: `WorkerController::presentSceneJson(json)` —
  the `presentDemoSlide` machinery generalised: parse, prepare with
  `fAutoPresentScene`, present the revision the worker acknowledges. The
  panel's Present button + `following` mode (navigate follows onto the
  output once presenting has started) is all the UI this task needs;
  rename/reorder/groups stay M4.
- **Panel** (`Main.qml`): operator column (status, layers, demo scenes,
  preview — unchanged) plus a lyric panel: name field + New/Remove song,
  song list, Add/Remove slide, slide list (click to select, present when
  following), the text editor, ◀ Prev / Next ▶ / Present / follow,
  Undo/Redo/Save/Load, and a status line showing dirty state and the
  service directory. Verified live (window grabbed under
  `QT_QPA_PLATFORM=xcb`): clean QML run, honest disabled states.
- **Verification**: `document_controller_tests` (Qt Core, scratch dir):
  editing refusals, debounced-commit semantics, compiled hand-off (the
  scene JSON satisfies the strict `scene::fromJson` validator and names
  the right slide with the typed lines), navigation clamps, commit-silence
  on the editor signal, undo/redo re-anchoring, and the full save/load/
  autosave startup matrix. `document_present_tests`: the acceptance act
  against a real worker (two slides + an undone present; clean shutdown).
- Gates: desktop **17/17**, headless **12/12**.

## Task 3.4 log (2026-10-24) — output controls and acknowledged live state: DONE

- **Deliverable**: clear controls and live indicators that match the
  output. The design keeps every promise of the presentation model: a
  clear is not worker state but a **derived presentation** — the scene
  JSON on air with one layer's content removed, compiled on the frontend
  and put through the ordinary prepare → present → acknowledgement chain.
  Revisions stay immutable, the worker's store remains the only claim on
  air, the untouched layer rides through byte-identical (its pixels
  cannot move), successive clears compose in either order, and *restore*
  is just re-presenting the revision the clear sequence cut from — the
  store still holds it, so no re-prepare, and if the worker has pruned
  it, its refusal is the honest answer.
- **`SceneEdit`** (new, in `openshow::ui-core`): `withWordsCleared`
  (items → `[]`), `withBackgroundCleared` (`backgroundColor` → 0,
  `background.image` → false, asset strings blanked — the reference's
  plain black with nothing left behind), `clearedSceneJson` (the scene
  library's cleared scene, serialized), and `sceneContent` answering the
  indicator question — words if items are non-empty, background if the
  fill is non-black or carries an image. Edits are minimal in-place JSON
  transforms, so every other wire field (provenance, style, the other
  layer) carries over verbatim and the strict validator still accepts
  the result.
- **Acknowledged live state** (`WorkerController`): the controller now
  mirrors the JSON sent with every `scene.prepare` against the revision
  the acknowledgement hands out (`fSceneJson`, pruned to a 32-revision
  window keyed by revision, since QHash order is not insertion order);
  `wordsOnAir`/`backgroundOnAir` recompute **only** when `output.status`
  reports the active revision — the indicator lights when the audience
  has it, not when the operator clicked. `clearSlide`/`clearBackground`/
  `clearAll` require a known JSON for the on-air revision (the initial
  revision 0 has none, so clears before the first present are no-ops);
  the first clear of a sequence records the restore point, presenting
  new content (`presentSceneJson`/`presentDemoSlide` through the shared
  `presentSceneInternal` split) retires it, and `restoreOutput` consumes
  it. The whole mirror resets on both worker-disconnect paths.
- **Panel**: a clear row between navigation and history — Clear slide
  enabled only while words are on air, Clear background only while a
  background is, Clear all ↔ Restore output toggling exactly once the
  output is acknowledged-clear (the reference's button behaviour), and
  an "on air: background + words / output clear" summary line.
  Esc / F1 / F2 follow the reference's presenter keys (Esc clears all —
  and restores when already clear). Live check: fresh session shows all
  three buttons disabled and "output clear" — honesty verified visually.
  Fade note: clears are ordinary presents, so the media layer fades over
  the 800 ms track and words over the 500 ms text track; the reference
  fades clears over the media duration, the difference is a nuance of
  track choice, not of clear semantics.
- **Verification**: `scene_edit_tests` (Qt Core) pins the content flags
  (both layers / colour-only / black / garbage) and the transforms
  round-tripping through the strict `scene::fromJson`: words gone with
  the whole background and provenance intact, background black + asset
  strings blanked with every word intact, both orders compose, garbage
  in yields nothing presentable. `document_present_tests` grew the live
  dance: typed slide (words over black — background legitimately off
  air), a style-coloured scene on air, clear-slide acknowledged leaving
  the background, both directions independent, clear-all presented,
  restore re-presenting the remembered revision and reporting itself on
  air; the test's prepare/present/wait-for-active helpers were factored
  out of the original one-call hand-off.
- Gates: desktop **18/18**, headless **12/12**.

## M3 checkpoint demo (2026-10-24) — and the fade bug it caught

- **The demo** ran the operator's story through the real classes against
  a live worker and captured the worker's actual output frames over the
  shared-memory preview stream (the same stream the panel's monitor
  tile reads): create *Be Thou My Vision* as a two-slide service in
  `DocumentController`, save it, reload it in a fresh session ("loaded",
  clean), put each verse on air over a background image, then the clear
  sequence — clear slide (background left), restore, clear background
  (every word left), clear all (black), one restore from black (full
  slide back). Every step waited for the worker's acknowledgement, and
  the saved `service.json` was checked to hold both verses under the
  right slides. All assertions green.
- **Background preserved across lyric changes** — the checkpoint's
  clause, proven at the pixel level: the frames for verse 1 and verse 2
  differ in the words and in nothing else; the night-sky background
  rides through the change untouched (and it is the same asset path, so
  the 3.2 path-keyed cache means the worker never even re-decodes it).
- **The bug the demo caught**: the verse-2 frame showed *verse 1* —
  clock advanced, words unchanged. The 1.5e event-driven loop presents
  only on a reason, and nothing re-armed it while a crossfade ran: a
  fade's presentation drew exactly one frame at blend 0 (still the old
  picture), the loop slept, and the new scene never appeared — while
  `output.status` cheerfully reported it active. The CPU fixtures missed
  it because `--check-transition` drives `draw()` directly with a clock
  instead of living in the loop. The fix is small and honest:
  `TestScene::transitionRunning()`, and the frame loop re-arms
  `fNeedsPresent` after every drawn frame while a fade runs, so the
  crossfade animates (vsync-paced) and the loop goes quiet again when
  the blends complete.
- **The regression** (`live_fade_tests`, Qt-free, display-labelled)
  asserts the live contract from the frontend's side: fade red to green
  and the presented-frame counter must climb across the fade window and
  then plateau (it animates, it settles, it does not busy-loop), and
  the preview stream must settle on the *new* scene's mean colour, not
  the old one's. Re-running the demo after the fix: all seven frames
  now tell the honest story (full slide, full slide, background only,
  restored, words on black, black, restored from black).
- Gates after the fix: desktop **19/19** (the idle check from 1.5e
  stays green — the re-arm only fires while a fade runs), headless
  **12/12**.

## Task 4.1 log (2026-10-24) — main workspace and resizable panels: DONE

- **Deliverable**: the reference's application shell (M4 opens). The old
  two-column lyric panel becomes the three-region workspace the spec
  measures (docs/reference.md §2): a 40px `TopBar` (title left, the five
  page buttons centred with the `--secondary` accent underline, the
  output-status pill right in the reference's connected/disconnected
  greens/reds, and the 5x40 `rgba(255,0,0,.25)` unsaved marker at the
  left edge) over a row of `SidePanel` left dock (the Projects/song
  list), the Show centre (slide list + editor + nav/present/history),
  and the `SidePanel` right dock (always the preview, with the output
  tools below it — the reference's Preview + ShowTools). Everything M3
  built still drives the same `DocumentController`/`WorkerController`;
  nothing regressed, it just moved into the shell.
- **`WorkspacePanels`** (new, in `openshow::ui-core`, Qt Core only): the
  reference's resize is reproduced boundary-for-boundary in one pure
  `widthFor` — below 100.05 the panel collapses to the 4px handle,
  100.05–200.1 floors at 200.1, 270–310 snaps home to 290, above 638
  clamps — so the exact numbers an operator feels mid-drag are
  fixture-pinned with no window. Clicks restore a collapsed panel to the
  default and do nothing else (the reference never closes on a click);
  widths persist to `workspace.ini`, and a panel left closed reopens at
  290 on startup, matching `Resizeable.svelte`. `SidePanel.qml` turns
  pointer positions into candidate widths (the left dock uses scene-x
  directly, the right the width at press minus the travel — using the
  live width here would compound each event) and feeds them in; the
  maths stays C++.
- **Dark theme**: the default light controls sat wrong on the reference
  palette, so the app sets the Material style (dark, accent `#f0008c`),
  which makes the whole workspace read like FreeShow's flat dark UI.
  The theme tokens (§3) are applied throughout — `--primary` panels,
  `--primary-darkest` wells, `--primary-darker` canvas.
- **Fixed-size capture seam**: `OPENSHOW_UI_SIZE` opens the window at an
  agreed size so the layout can be pixel-probed (PLAN.md UI acceptance:
  "agreed captures at fixed sizes"). The default stays 1380x800.
- **Verification**: `workspace_panels_tests` pins every snap edge, the
  click rules, and the persistence/start-up rules against a temp
  `workspace.ini`. The layout was measured off a 1680x1020 capture (the
  size the reference figures were taken at), correcting for the 1.5x
  device pixel ratio: the top bar's bottom edge lands at 40.0 logical,
  the left panel's inner edge at 290.0, and the right panel's at 1390.0
  (1680−290) — all three exact. Writing `leftPanel=450`/`rightPanel=638`
  and relaunching showed the binary honouring both (edges at 450 and
  1042), proving the persistence path end to end. Live drags can't be
  scripted (no XTEST tooling), so the drag affordance is verified by the
  pure snap fixtures plus these persisted widths; the handle/arrow and
  click-to-open are exercised by hand.
- Gates: desktop **20/20**, headless **12/12**.

## Task 4.2 log (2026-10-24) — searchable libraries, playlist ordering: DONE

- **Deliverable**: the operator can find shows and media and arrange the
  service. The reference's bottom **library drawer** (`LibraryDrawer.qml`)
  spans the window: its 40px bar carries the eight tabs in order (Shows
  and Media live; the six whose milestones have not arrived render
  disabled) plus the 100px Search button that swaps for the 290px input
  on Ctrl+F (Esc clears and collapses, Ctrl+1/2 pick the enabled tabs),
  and the open body (300 default) runs the list plus the right-hand
  item-info column. The Shows tab holds the song library (create/remove
  moved there, per the reference — the left dock is now the show page's
  Projects analogue: playlist chips with a `+`, then the running order);
  the Media tab imports files into the domain's asset registry
  (`addAsset`, bytes joined by `save()`) and lists them. Height is
  `WorkspacePanels`' again: pure `heightFor` snaps a drag shut below 80
  and clamps at the viewport, closes remember the open height and reopen
  at `max(stored, 300)`, and — like the reference, whose store is
  per-session — a session that starts closed reopens at 300. The top
  edge drags with those rules; the bar click toggles.
- **Search** (`LibrarySearch`, pure Qt Core): reproduces the reference's
  contract from `utils/search.ts` — normalise both sides (lowercase,
  strip the `.,/#!?$%^&*;:{}=-_'"´`~()` set, NFD + drop diacritics,
  collapse whitespace), then a show matches only when EVERY query word
  matches at a WORD START in the title or content ("here" never matches
  "There's", every word narrows), with the documented confidence tiers
  (exact/leading title 100, all words in title 90, split 65,
  content-only 50) sorted stably. Quoted phrases, fuzzy typos and the
  CCLI/song-number exactness lean on fields OpenShowQt does not carry
  yet and are left out deliberately. Enter in the search field drops the
  first match into the playlist after the current show (the reference's
  placement rule) and opens it.
- **Controller surface** (`DocumentController`): `playlists`,
  `playlistEntries` (resolved names/slide counts), `currentPlaylistId`,
  `assets`, and history-wrapped `createPlaylist`, `selectPlaylist`,
  `addShowToPlaylist`, `insertShowInPlaylist` (clamped; −1 appends),
  `insertShowAfterCurrentShow`, `removePlaylistEntry`,
  `movePlaylistEntry` (toIndex = position after the move, clamped — the
  drag handles, ▲▼ buttons and drops all call it), `searchShows`,
  `importMediaFile`, `removeAsset`. Every session seeds one "Service"
  playlist directly at construction/load — baseline, not an undoable
  edit, so undo can never land on "no playlist" — and undo/redo
  re-anchor the current playlist when a restore took it away. Removing a
  show drops the entries naming it (the domain's cascade).
- **Ordering UX**: rows reorder by drag (`text/plain` `entry:<id>`, the
  drop side splits on the first colon) or the ▲▼ handles; drops from the
  drawer (`show:<id>`) insert at the gap nearest the pointer (single
  DropArea, `Math.round((drop.y − list.y) / 30)`, shifted by one around
  the moved row). Double-click a library row appends it. The handles
  exist because drags cannot be scripted here — they guarantee the same
  move the reference only offers via drag, and the move semantics
  themselves are fixture-pinned. Sections ride along in the row data
  (header rendering is 4.3+). The left dock's old songs list is gone —
  the drawer is the reference's place for it.
- **Deviations noted**: no categories column (no tag concept in the
  domain yet); drawer widths follow the window instead of a third pair of
  persisted widths; seeded playlist is not undoable (baseline by design);
  the drawer remembers its open height in memory only, matching the
  reference's per-session store.
- **Verification**: new `library_search_tests` pins the normalisation,
  the word-start contract and the tiers/ordering; `document_controller_tests`
  gained a playlist/library pass (seed, add/insert/Enter-placement,
  move/clamp, undo/redo, multi-playlist isolation, the delete cascade,
  media import/undo/redo/remove, search integration, save/load of the
  arrangement); `workspace_panels_tests` pins `heightFor` and the
  close/reopen/persist rules. Live: a 1680x1020 capture shows the open
  drawer (tabs, Search button, info column, list empty state) and the
  new left dock; writing `drawerHeight=40` and relaunching pixel-verified
  the closed bar at logical y=980 with tabs and Search showing. Tab
  clicks, the search swap and drags stay hand-tested (no XTEST); their
  logic is all in the C++ fixtures.
- Gates: desktop **21/21**, headless **12/12**.

## Task 4.3 log (2026-10-24) — object selection, move and resize: DONE

- **Deliverable**: every slide carries an authored text box in the
  1920x1080 space, and the operator can select it, drag it, resize it,
  nudge it and type exact numbers — each edit one undo step, and the
  renderer honours the box per item.
- **Domain**: `Slide` gains `x/y/width/height` whose defaults
  (160/100/1600/880 — the canvas minus the lyric margins, centred exactly
  where M2/M3 centred the canvas block) make an untouched slide draw
  pixel-identically to pre-4.3; `setSlideGeometry` refuses non-finite
  edges and non-positive sizes, leaving the accepted box untouched.
  Storage always writes the four numbers; the loader's
  `wantOptionalNumber` lets pre-box ("legacy") manifests read with the
  defaults, and a loaded box that cannot be placed is refused naming its
  slide.
- **Scene library**: `TextItem` carries the box, `compileSlide` copies the
  slide's verbatim, and the wire format *requires* `x/y/width/height`
  (strict `wantBoxEdge`/`wantBoxSize`). `scene_tests` pins the domain
  defaults against the scene constants (`kCanvasWidth`, `kDefaultBox*` —
  one promise written twice), the box's centring, and that a wire item
  missing its box or holding a zero-width one refuses.
- **Renderer** (`SceneText`): `layoutScene` lays out every item inside
  its own box — wrapped at the box width, per-item shrinkToFit with the
  4-pass convergence floored at `kSceneMinFontSize` — and `drawLayout`
  centres each paragraph in its box, vertically as well as horizontally.
  The shared block (accumulated heights, shared gap, shared scale) is
  gone; `SceneLayout` is just the laid-out items. The fade `saveLayer`
  bounds are the union of occupied boxes outset 8px — accumulated by
  hand, because `SkRect::Inverted()` does not exist in this Skia fork.
  Fixtures: the fit test now asserts per-item shrinking (size below the
  authored 260, paragraph within the box height, a calm stanza keeping
  exactly 72), and the new per-box test pins two boxes drawing as two
  separated bands whose ink centroids land at their own box centres
  (250/830 authored, at scale), with boxes travelling through layout and
  fitting items keeping their own sizes.
- **Controller**: `slides()` rows carry `x/y/width/height` (QML binds the
  current box as `docs.slides[docs.currentSlide]` — re-evaluating on
  `documentsChanged` *and* `navigationChanged`), and
  `setSlideGeometry(x, y, w, h)` applies to the current slide through
  `edit()`: one call is one history entry, so a drag commits once on
  release. The fixture pins the row's default box, an accepted edit, one
  undo reverting the whole box, redo, a refused degenerate box (status
  naming "text box", box untouched) and refusal with no current slide.
- **UI**: `Main.page` ("show"/"edit") drives the centre; the TopBar page
  buttons are live for those two (stage/draw/settings stay dimmed), and
  window **Ctrl+1..5** switch pages. The Ctrl+1/2 pair cedes to the
  drawer's tab shortcuts exactly while the keyboard is inside the drawer:
  `libraryDrawer.containsFocus` gates both owners, because two enabled
  shortcuts on one key cancel each other out in Qt — and
  `Qt.ShortcutItemContext` turned out not to exist in this Qt build, its
  assignment silently falling back to window scope (journald is where
  `openshow-ui`'s QML errors surface). New **`EditCanvas.qml`**: a 16:9
  stage (zoom = width/1920), a text representation wrapped and shrunk to
  the box (`Text.Fit`), the selection outline plus four corner handles;
  dragging inside moves (clamped to the canvas), a corner resizes around
  the opposite corner (≥20 authored px), and either commits whole
  authored pixels on release; clicking empty canvas deselects; **arrows
  nudge 1px / Ctrl+arrows 10px** (the reference's EditTools rule) when a
  selection is live and nobody is typing; the first **Escape** drops the
  selection, the second reaches the output clear-all (again gated as a
  mutually exclusive pair). Below the canvas an **X/Y/W/H** row is the
  deterministic path: the field that loses focus commits all four, and a
  value that does not parse or that the domain refuses snaps the fields
  back.
- **Deviations noted**: the X/Y/W/H fields sit under the canvas on the
  edit page rather than in the right dock, which 4.1 fixed as
  preview + output tools — edit controls belong beside their target; the
  canvas shows a representation (fixed 96px base through `Text.Fit`),
  not the style-resolved render — style fidelity on the edit page is
  4.4's job; drags clamp the box into the canvas while the numeric fields
  allow authored off-canvas positions; arriving on the edit page selects
  the slide's single item (deselect via empty-canvas click or Escape).
- **Verification**: the fixtures above; live, 1680x1020 captures show the
  edit page drawing slide one's authored 480/240/960/480 box offset on
  the stage with handles and its text centred inside it, and the X/Y/W/H
  fields reading exactly those numbers — an earlier round exposed that
  nothing synced at startup (the document loads before any QML exists, so
  no signal fires; fixed in `Component.onCompleted`), and the shipped
  binary's journal shows no QML errors from the new code. A two-slide
  sample service ("Capture Hymn", the first slide at the authored
  480/240/960/480 box) now lives in `~/.local/share/openshow-ui/service`
  for the hand-testing session. Drags, nudges and field edits stay
  hand-tested (no XTEST); their semantics are pinned by the controller
  fixture.
- Gates: desktop **21/21**, headless **12/12**.

## Task 4.4 log (2026-10-24) — typography, layering, reusable styles: DONE

- **Deliverable**: styles carry typography (horizontal alignment and a
  line-box multiplier) beside their colours/font, the edit page gains a
  STYLE panel and a background-image picker, and editing one style
  changes every slide that resolves through it — the acceptance, pinned
  end-to-end through the compiled-scene path.
- **Domain**: `Style` gains `align` ("left"/"center"/"right") and
  `lineSpacing`, whose defaults ("center", `kDefaultLineSpacing` = 1.25
  — the renderer's own line-box factor) keep pre-4.4 styles drawing
  identically. `createStyle`/`updateStyle` take the two values before the
  trailing `error` — no defaults, so every call site was forced into the
  review — and share one `validTypography` validator (size finite ≥0,
  align the three words, line spacing finite within 0.25..8).
- **Storage**: always writes both fields; the loader reads them
  optionally and *validates* them ("holds typography the renderer cannot
  honour", naming the style). A trap surfaced via the fixture:
  `wantOptionalString` **blanks** an absent key, so a legacy style's
  align arrives empty — the loader now defaults empty to "center"
  explicitly (an absent align is exactly how a pre-typography style says
  centred). `sameService` compares the two new fields.
- **Scene library**: `TextItem` carries `align`/`lineSpacing` (defaults
  matching the domain's — `domain::kDefaultLineSpacing` is used directly,
  one promise once); `compileSlide` copies the resolved style's
  typography (built-in centre/1.25 with no style anywhere); the wire
  *requires* both (`wantAlign` in the three-word set, `wantLineSpacing`
  within 0.25..8), and the two hand-written payloads in `rpc_ipc_tests`
  and `live_fade_tests` grew the fields like 4.3's box did.
- **Renderer** (`SceneText`): `layout()` maps `item.align` to the
  paragraph's `TextAlign` (unknown → centred, guarding hand-built items)
  and takes the line-box multiplier from `item.lineSpacing`, clamped to
  0.25..8 with a NaN-safe `!(x >= 0.25f)` guard; the old
  `kLineHeightFactor` constant is now a local mirror of the domain
  default, so fallback and model cannot drift.
- **Controller**: `styles` (list), `currentStyle` (the resolved map —
  override or show default, empty when neither) and
  `currentStyleIsOverride` properties; five undoable ops —
  `applyStyleToCurrentSlide` (empty id follows the show again),
  `setShowDefaultStyle`, `editStyle` (appearance first, then rename, all
  in one history entry; blank names refused before anything records),
  `createStyleFromCurrent` (forks the resolved look and overrides the
  current slide — create+apply are one entry, and a refused apply
  removes the style it made), and `setSlideBackground` ("" clears; the
  domain refuses unknown assets). `slides()` rows carry
  `backgroundAssetId`.
- **UI**: a 250px **STYLE panel** right of the edit canvas: style
  ComboBox (row 0 "— show default —" is the empty id), **New / Show
  default / Follow show** buttons, and Name / Text colour / Background /
  Font / Size / Line-spacing fields plus **L·C·R** align buttons — all
  commit the whole panel on focus-out (align buttons on click) through
  `editStyle` and snap back on refusal, the box-field pattern; a slide
  following *nothing* creates its style on first edit (and the creation
  is undone too if the values refuse). A **BACKGROUND IMAGE** combo
  (None + media assets) drives `setSlideBackground` — the
  colour→image→words layer stack. `EditCanvas`'s representation now
  binds the resolved style: family, colour, size (0 → 96 cap),
  alignment, `lineHeight`, and the stage paints the style's background
  colour — closing 4.3's noted deviation.
- **Deviations/caveats**: the panel edits the *resolved* style (that is
  the reuse; per-slide change means forking with New); Material floats a
  non-empty field's `placeholderText` as a ghost label above the box, so
  panel fields carry no placeholders (the headers name them), and two
  inline components (`PanelField`, `PanelButton`) replace Material's
  too-wide/too-tall defaults in the column; `lineHeight`/`lineHeightMode`
  live on `Text` directly, not under `font`, in this Qt build (qmlcache
  stays silent — journald caught it).
- **Verification**: fixtures — domain (update carries typography,
  refuses bad align/spacing, manifest round-trip, legacy style loads
  centred at 1.25, un-honourable align/spacing refused naming the style);
  scene (compile passthrough on all three resolution branches, wire
  refuses missing/unknown align and collapsed/exploded spacing);
  scene_render (left ink hugs the box's left edge, right hugs right, the
  default straddles; measured band pitch 62.5px = 100×1.25×0.5 and
  lineSpacing 2.5 doubles it); controller (one `editStyle` lands the
  same colour/align/spacing/size on **both** slides' compiled scenes,
  undo/redo it, fork-edit-undo-back, follow/pin, every refusal silent on
  the model, background set/clear + row + compiled `hasImage`, the
  no-current-slide refusals). Live 1680x1020 capture (via a temporary
  default-page patch, reverted): the "Capture Hymn" service now carries
  a left-aligned 64px `#ffd23f`-on-`#101828` "Bright" show default — the
  edit page paints the navy stage, amber text left-set in the authored
  480/240/960/480 box, L lit on the panel, fields reading the style, and
  Follow show correctly dimmed with no override. The shipped binary's
  journal shows no new QML errors (the two pre-existing warnings —
  the shared Ctrl+1 binding and the demo Flow — stand). ComboBox clicks,
  forks and the background picker stay hand-tested; their semantics are
  the controller fixture's.
- Gates: desktop **21/21**, headless **12/12**.

## Task 4.5 log (2026-10-24) — shortcuts, clipboard, essential settings: DONE

- **Deliverable**: the running show works from the keyboard — presenter
  keys walk the current show straight onto the output, the core document
  keys (undo/redo/save) and the slide clipboard (copy/cut/paste/duplicate)
  sit beside them, and a real settings page (Ctrl+5, enabled in the top
  bar) persists the essentials: presenter number keys and window
  fullscreen, plus the shortcut cheat sheet that makes the whole scheme
  learnable in-app.
- **Controller clipboard**: `DocumentController` grew
  `copyCurrentSlide`/`cutCurrentSlide`/`pasteSlide`/`duplicateCurrentSlide`
  and a `canPaste` property. The clipboard is *internal to the session*
  — the reference's clipboard carries slides, and system copy & paste
  stays with the text fields (which is also why `QClipboard` never
  enters this Qt-Core-only class). A clip captures the whole
  presentation: lines, notes, box, style id, background asset. Paste
  inserts a fresh slide right after the current one, carries every
  field through `insertSlide` + the existing setters inside one
  `edit()` — one undo entry, with the inserted slide removed again if
  any carry step refuses (History does not roll refused mutations back
  for you) — and selects it. References deleted since the capture are
  *dropped, not fatal*: a style or asset that no longer exists simply
  does not ride along. Cut is capture + the existing removal (one
  entry); duplicate reads the current slide directly and never touches
  the clipboard. Fixture: 30+ assertions — empty-clipboard refusal,
  content identity through the compiled scene (colour/align/size/
  line-spacing/background), selection placement, one-undo-per-entry,
  clipboard surviving undo/redo and duplicate, the stale-asset paste,
  and the no-slide refusals each naming themselves.
- **`AppSettings`** (Qt Core, ui-core): two persisted toggles
  (`numberKeysPresent` — off like the reference, `fullscreen`) in the
  same `workspace.ini` as the panel widths, injectable path for the
  fixture, sync-on-write since toggles are rare. Exposed to QML as
  `appSettings` beside `docs`/`worker`/`panels`; `workspace_panels_tests`
  now pins defaults and round-trip (one flipped, the other untouched).
- **Presenter keys (Main.qml)**: Space/PageDown/Right/F5 advance,
  PageUp/Left step back, Home/End jump, "." clears all — each select +
  present, the exact pair a Present click performs. Gating is the
  reference's: typing keeps its keys; a focused `AbstractButton` or
  `ComboBox` (which eat Space) and the library drawer (which owns
  arrows) silence the block; and the edit page's selected box owns
  Left/Right through `nudging` — presenter arrows enable precisely when
  `presenterNav && !nudging`, mutually exclusive like the Escape pair.
- **Document keys**: Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z / Ctrl+S and the
  clipboard letters, all gated `!typingIntoField` so text fields keep
  their native meanings (Ctrl+Z inside a field must undo *text*); Ctrl+S
  is ungated — no field wants it, and every controller call flushes the
  pending edit first. Ctrl+D follows the reference's selection rule:
  duplicate when there is a current slide, drawer toggle when there is
  not — one pair of mutually-exclusive shortcuts. (Ctrl+C/X/V are bound
  as literals rather than `StandardKey.*`: the standard keys carry
  Ctrl+Ins-style aliases nobody here uses and each logs a binding
  warning at startup.)
- **Number keys and F11**: 1..9 play slide one..nine and 0 plays ten,
  enabled only by the setting and silenced while typing, in the drawer
  or on the edit page; F11 flips the window (and persists), so a
  projector setup survives restarts.
- **Settings page**: `page: "settings"` now has content — PRESENTER
  checkbox (number keys), WINDOW checkbox (fullscreen), a 14-row
  SHORTCUTS cheat sheet, and SERVICE rows (version + live worker status,
  service directory, autosave note). Ctrl+5 is live (drawer tab cedes,
  like Ctrl+1/2), the top-bar button is enabled, and stage/draw stay
  honest-disabled. Captures at 1680x1020: the show page unchanged, the
  settings page rendering every section; the journal shows no new QML
  errors (the two pre-existing warnings — Esc's shared standard-key
  binding and the 2.4c demo Flow — stand).
- **Hand-test list grows**: presenter keys + "." + F5 in and out of the
  edit page (with and without a selected box), clipboard chords on the
  show page, Ctrl+D both states, number keys after enabling the
  setting, F11, and the settings checkboxes surviving a restart.
- Gates: desktop **21/21**, headless **12/12**.

## M4 checkpoint (2026-10-24) — core operator workspace and editor

- **The demo** ran the full M4 story through the real classes against a
  live worker, capturing the worker's actual frames on the preview
  stream: author a three-slide song (words, an authored 960/540 box, a
  "Bright" style with colours/size/left-align, a background image, an
  interlude), save it, reload in a fresh session, run the clipboard
  (copy→paste→duplicate with one-undo-per-entry), then walk the
  presentation the way the 4.5 keys do — select + present — and
  exercise clear-all and restore. All 37 assertions green.
- **Pixel proofs**: the style's amber words and navy background are
  really on the output; a mid-service words edit changes the text band
  while the background strip above the box is *identical* across the
  two frames (≤1/255 per channel); clear-all lands on true black; and
  the restore returns a frame whose background strip and amber coverage
  match the original slide's.
- **What the demo's first runs flagged** (all three were demo-side
  expectations, no app bug — recorded because each names a real
  contract): (1) a reloaded scene is *not* byte-identical across
  save/reload — `background.assetPath` changes, because save **archives
  the media into `<service>/assets/`** (PLAN storage rule: copy media,
  reference the kept copy); the honest assertion is "identical with the
  asset path set aside" plus "the archived file exists inside the
  service". (2) Two undos after paste+duplicate legitimately land
  navigation on the clamped index (the interlude), not on slide 1 —
  history entries and navigation position are separate facts. (3) The
  black→slide restore rides the media-timed fade, so the frame settles
  after ~800 ms, not ~500 ms — the pump must outlast it.
- **Milestones**: 4.1 shell + panels, 4.2 library drawer + playlist
  ordering, 4.3 selection/move/resize, 4.4 styles + layering, 4.5
  keyboard/clipboard/settings are all DONE, committed and pushed.
  Gates at the tag: desktop **21/21**, headless **12/12**.

## Task 5.1 log (2026-10-25) — production bounded decode queue: DONE

- **What graduated** (5.1a): the 1.5e bench's improvised glue is now
  `openshow::worker::PlaybackPipeline` (`src/worker/playback/`, new
  Qt-free static lib `openshow::worker_playback` linking worker_gpu +
  worker_media). It owns `VideoFrameSource`, `GpuYuvUploader` (as the
  ring's `FrameBacking`), the paced-submission thread (supply lookahead,
  loop-pts continuity across decoder wraps) and the session's
  `VideoPresenter` role (per-token pts under the stats mutex, the
  missed-window threshold, uploader release/drop). GPU mode takes a
  `Gpu` triple set (from `OutputSession::gpuHandles()` or a headless
  `VulkanContext`); CPU mode (`openCpu`) runs the plain-memory ring with
  copying decode so the queue contract is testable with no GPU. Clock
  and sink are injected callables — with a session the sink is
  `submitVideo` and the clock is the audio stream time; default clock is
  wall from `start()`. `requestStop()/join()` bake in the bench's proven
  ladder: stop flags → ring stop (wakes a parked poll) → decode unwind →
  uploader shutdown (wakes a commit parked on a held slot).
- **Seam extraction**: `VideoPresenter`/`VideoSubmission` moved out of
  `OutputSession` into `playback/VideoSeam.h`; `OutputSession` keeps the
  names as `using` aliases so every existing caller is untouched. The
  pipeline header needs no window machinery. `VideoFrameSource` gained a
  const `ring()` overload for the stats aggregator.
- **Bench graduation** (5.1b): `runAvBench` keeps probe, audio sink and
  clock choice, the optional preview consumer and reporting; the private
  upload thread, `BenchPresenter` and `pickYuvColorSpace` left
  `main.cpp`. Parity reruns on the pipeline: `av-10s.mp4 --preview`
  300 presents / **0 missed** / 0 drops / drift mean +0.8 max 2.4 ms /
  ring peak 3 of 4 / 304 vkCmdCopyImage with zero CPU copies;
  `decode-hd.mp4 --no-audio --vulkan-validation` 150 presents /
  **0 missed** / drift mean +1.5 max 17.1 ms.
- **The new test** (5.1c): `playback_queue_tests` (ctest, both presets;
  generates its own fixtures, skips 77 without the ffmpeg CLI or a
  Vulkan device). CPU: healthy 60-frame drain at depth 3
  (peak ≤ 3, monotone sequences, clean EOF); frozen-clock stall at
  depth 2 — submissions provably freeze, `peakFilled` stays at 2,
  the producer does not race through the clip, and releasing the clock
  delivers all 100 frames in order; prompt stop from parked states.
  GPU (headless device): healthy direct-decode drain (wrapped-YCbCr
  image per submission, four slots, one copy per frame, zero CPU
  copies) and a stuck presenter holding every token. Green with zero
  Vulkan validation errors, and again under `OPENSHOW_VULKAN_VALIDATION=1`.
- **What the stuck-presenter scenario taught** (test-side expectation,
  recorded because it names a real contract): with a single decode
  producer that re-claims the ring slot the pacing thread released
  *before* the presenter returned its GPU twin, the producer parks on
  that token's next commit — so with a stuck frontend the pipeline can
  park at 1 submission, not `depth`. How many tokens escape before the
  first reused slot is a race; that submissions stop growing, the ring
  stays at depth, the wait is not an error, and the shutdown wakes the
  parked commit — those are the pinned contract. Production is
  unaffected in the other direction: the session releases every token
  it drew (or drops it), so commits never wait longer than one draw.
- **Decisions**: (1) the pipeline references no `OutputSession` — the
  narrow `Gpu` struct + injected sink/clock keep it drivable headless
  and testable with a fake consumer; (2) `missed` stays a pipeline stat
  with the bench's 25 ms window (`setMissedWindowMs`), because M6.2
  diagnostics will want the same counter; (3) CPU mode is the
  explicitly-compatible copying path end to end — it exists to prove
  the queue, not to decode for output; (4) the presenter callbacks
  share the pipeline's one stats mutex (same lock order as the bench:
  stats mutex → uploader sync, never the reverse).
- **Checks**: desktop `ctest --preset desktop` **22/22**; headless
  `ctest --preset headless` **13/13** (`playback_queue_tests` counted
  in both; `worker_is_qt_free` green with the pipeline linked in).
- **Next**: task **5.2** — video controls, seeking and looping over the
  pipeline (play/pause/seek/loop as pipeline operations with flush
  semantics; the 5.2 acceptance sentence is playback transitions and
  seek flushing passing tests).

## Task 5.2 log (2026-10-25) — video controls, seeking and looping: DONE

- **Goal**: PLAN 5.2 — "Video controls, seeking and looping; acceptance:
  playback transitions/seek flushing pass tests". `play()`, `pause()`,
  `seekTo()`, `setLoop()` and `position()` became real `PlaybackPipeline`
  operations acting through the existing decode/pacing seams, not a
  control shell beside them.
- **Approach**:
  - *Seek inside the decode thread*: `VideoFrameSource::requestSeek`
    stamps the target and counts the request up; between frames the run
    loop applies it — `av_seek_frame` BACKWARD (stream time base),
    `avcodec_flush_buffers`, the new `FrameRing::discardReady()` purge of
    everything visible, and a skip-until-target gate that runs **before**
    `claim()` in `emitFrame`, so pre-target frames never occupy a slot.
    `seekRequested()`/`seekGeneration()` are the pacing thread's flush
    protocol: `requested != applied` means "flush in flight, drain, present
    nothing", and the generation is published only *after* the purge, so
    any frame polled once both counters agree post-dates the flush.
    Consecutive requests coalesce. Loop rewind clears the skip gate.
  - *Pause in the pacing thread*: pause stops polling, so the ring fills
    and the producer parks on `claim()` — the queue's own bound doubles as
    the pause parking spot, the media position holds exactly, and no
    unbounded decode-ahead exists to become stale.
  - *Clock re-anchoring*: pause and seek both break "pts 0 at clock 0".
    Deadlines now run against `pts + clockOffset + loopBase`, and resume
    or a landed seek anchors the first following frame due *now* — the
    right shift for a running wall clock and a no-op for an audio clock
    that paused or jumped with the media (the session's wiring, 5.4).
    `fPtsByToken` now stores the deadline, so drift still means
    "late relative to when it was due".
  - *Backward-seek regression pinned*: the effective-pts loop-wrap
    heuristic (`pts + 0.5 < lastPts`) would have read a backward seek as
    a loop and presented at target + clip length; the landed-seek path
    resets the wrap base instead. A dedicated scenario asserts the tail
    stays at the target on a 4 s clip.
  - *Live loop toggle* reaches the decode thread's EOF decision; loop-pts
    continuity keeps working across the wrap and the toggle-off finishes
    the clip.
- **Decisions**: (1) the pacing thread drops at most one frame around a
  landed seek (polled-before-purge frames cannot be told apart from fresh
  ones by generation alone) — bounded, documented, test-tolerated with a
  skip-one slot; (2) a seek issued while parked applies when playback
  moves again rather than force-waking the decode thread — the parked
  scenario pins the observable contract (nothing presents, resume shows
  the target); (3) `seekTo` clamps negatives and the pacing thread, not
  the source, owns the clock anchor — the pipeline never mutates the
  injected clock; (4) `position()` is the last *submitted* effective pts
  (frozen across pause, drops backward across a backward seek — it is
  what the operator has seen).
- **Checks**: new `playback_control_tests` (CPU mode, wall clock,
  ffmpeg-CLI fixtures, skips 77 without the CLI) — five scenarios:
  pause/resume freeze and continue with `missed==0`; forward seek
  (seeks==1, ≥30 skipped pre-target, tail presents at target);
  backward seek (tail ∈ [0.45, 1.1] after seeking 1.2→0.5 on the 4 s
  clip — pins the +clip regression); seek-while-paused; loop wrap with
  climbing effective pts then toggle-off to EOF. Ran green twice
  back-to-back headless (a `bool ok = ... && ok` self-use in the test —
  not the pipeline — caused the first exit-1 and is fixed). Gates:
  desktop **23/23**, headless **14/14**. `--av-bench av-10s.mp4
  --preview` rerun unchanged: 250 presents, **0 missed**, drift mean
  +0.8 ms max 2.4 ms, ring peak 3/4, loops=1 with pts continuity.
- **Next**: task **5.3** — PipeWire output and stereo mixing (independent
  volume and mute for video audio and standalone audio files).

## Task 5.3 log (2026-10-25) — PipeWire output and stereo mixing: DONE

- **Goal**: PLAN 5.3 — "PipeWire output and stereo mixing; acceptance:
  video and standalone audio have independent volume and mute". The
  mixing stage became a production component, `openshow::media::AudioMixer`
  (Qt-free, PipeWire-free, FFmpeg-free), sitting in front of the one
  `PipeWireSink`; `--av-bench` proves it against the live daemon.
- **Approach**:
  - *Bounded inputs, one consumer*: each input is a fixed-capacity SPSC
    `AudioRing` (default 96000 frames), registered before `start()`
    (kMaxInputs=8, the table is frozen for the mix thread). A feeder
    paces on short writes exactly as it did against the sink directly,
    so nothing grew when the stage was inserted — and no input can grow
    its queue (the M5 boundedness rule carries through).
  - *The mix thread*: 10 ms blocks; each input's frames are read
    (shortfall zero-padded, `starvedBlocks` counted per input — silence
    flows, the output never stalls), gain is applied interpolated per
    frame, the sum goes out through the injected `WriteFn` with
    retry-sleep pacing (the sink's full ring paces the mixer the same
    way the mixer paces the feeders).
  - *Independent volume and mute*: control-plane atomics read per block;
    changes ramp over 50 ms (`kRampSeconds`) so no step can click; a
    muted input keeps draining, so unmute resumes live audio rather
    than dumping a backlog.
  - *Real seams in the test*: `audio_mix_tests` runs two real
    `AudioFileFeeder`s decoding sine wavs through the mixer into a fake
    output device (ring + drain thread at nominal rate, zero-padding
    shortfall — the PipeWire callback's discipline), so the decode →
    resample → mix → deliver chain is exercised without PipeWire.
  - *Live proof*: `--av-bench` rewires sink ← mixer ← video feeder
    (+ `--second-audio FILE` as input 1); a failed input degrades to
    silence with the output still up; mixer/per-feeder counters report
    with the run; teardown feeders → mixer → sink.
- **Decisions**: (1) the input table is fixed at `start()` — the mix
  thread walks it without locks, and hot-plug can wait for a need;
  (2) starvation is *per input* and silent-safe (the sink's own
  underrun counter stays meaningful: it now proves the mixer always
  fed it even during total input starvation); (3) no `resetInput()` —
  clearing a producer's ring from the consumer side would race the
  SPSC contract; seeks will flush from the source side when 5.4 wires
  them; (4) the fake-device latency bound (100 ms ring, 400 ms settle)
  is what makes capture-window assertions exact despite the pipeline.
- **Findings on the way** (test-only unless noted): the fake device's
  drain needed real 10 ms pacing (busy-draining recorded garbage); the
  ffmpeg 9.0.2 CLI's `sine` source emits amplitude 0.125 and this
  swresample attenuates mono→stereo upmix by 1/√2 — the fixtures are
  now written in C++ (stereo s16, exact amplitudes) so the expected
  RMS is analytic; and a real bug in the bench wiring, fixed here: the
  feeder callbacks captured `videoInput`/`extraInput` by reference out
  of a block the feeder threads outlived — the second input wrote to a
  garbage id and stayed silent (the by-value capture is now commented).
- **Checks**: new `audio_mix_tests` (both presets; no env skip — no CLI
  or daemon dependency): 40 checks — DC mix `out = vA·A + vB·B` across
  five volume/mute combos with asymmetric channels (tol 1e-4); clickless
  ramp (max sample jump 0.0004 for live 0→1, settles at 1.0); starved
  input counted, silence passthrough, late joiner flows with no penalty,
  zero device underruns; feeder scenario RMS 0.768 → 0.300 (A muted) →
  0.768 (unmuted), muted feeder keeps feeding. Green twice back-to-back.
  Gates: desktop **24/24**, headless **15/15**. Live benches:
  `--av-bench av-10s.mp4 --preview --monitor -1` parity (250 presents,
  **0 missed**, drift mean +0.8 ms max 2.2 ms, sink underruns 0, mixer
  input 0 written=575040); with `--second-audio media-fixtures/probe.wav`
  both inputs mix (extra written=390720, loops=8 of the 1 s wav),
  underruns 0, 0 missed. The 201 shared starved blocks are the startup
  sprint prefilling the sink's own ring.
- **Next**: task **5.4** — playback clocks and synchronization (the
  session's A/V sync: audio clock drives video, acceptance: A/V timing
  meets the PLAN targets).

## Task 5.4 log (2026-10-25) — playback clocks and synchronization: DONE

- **Goal**: PLAN 5.4 — "Playback clocks and synchronization; acceptance:
  A/V timing meets the targets" (M5 checkpoint: audio clock when audio
  is present, monotonic otherwise; steady A/V offset < 50 ms, < 1%
  missed deadlines). Graduated into production `openshow::worker::AvPlayer`
  (Qt-free, `playback/`): the one component that owns sink + mixer +
  feeders + attached pipelines as a transport, so `--av-bench` drives
  real seams rather than bench glue.
- **Approach** — the sync invariant, expressed as start order:
  - *Queue depth is latency, never skew*: the sink connects with
    `PW_STREAM_FLAG_INACTIVE`, the server consumes nothing until
    `resume()`. Start order is sink (inactive) → feeders → pre-roll
    (every feeder has written ≥ `preRollFrames`, default 100 ms, within
    a ≤ 2 s window) → mixer → `sink.resume()` — and resume is exactly
    where the media clock anchors, so the first frame the server pulls
    is real content at the clock's zero. Against the 5.3 baseline (which
    pre-filled ~2 s of silence, `starved=201`) the audience now hears
    content from frame one: `starved=0`.
  - *Pause/resume never flush*: pause freezes the clock first, then
    deactivates the stream; the bounded rings park the feeders and that
    pile-up IS the seamless continuation. Flushing on resume would punch
    a hole of exactly the unheard queue.
  - *Seek is the only re-anchor*: `AudioFileFeeder::requestSeek` (latest
    request wins) applies on the decode thread — backward container seek,
    decoder flush, resampler rebuild, pre-target frames resampled into
    silence and counted (`seekDiscards`), never dropped; the emit retry
    loop aborts on a pending seek so a sprinting feeder parked on a full
    ring cannot delay it. The feeder's `seeked` callback fires at the
    instant it is quiet — the race-free moment for the owner to drop that
    input's queue via `AudioMixer::requestInputFlush` (mix-thread side,
    counted as flush not starvation, consumer-side `AudioRing::dropAll`).
    After all feeders report `waitSeekApplied`, AvPlayer flushes the
    sink queue (lands on the next process callback; while paused, on
    resume — disclosed fuzz: ≤ 1 mix block + 1 server slice at a flush
    boundary, transient) and rebases the clock to the target.
  - *Clock transport*: `AudioClock` gained pause/resume/rebase/anchor —
    pause latches `raw − offset`, resume folds the stale-extrapolation
    overshoot into the offset so the position continues from the hold
    without a jump; a rebase while paused sets the hold only. With no
    daemon (`enableSink=false`) one monotonic clock anchored at start
    drives everything — degrade, never fail.
- **Live bug found while proving this**: `pw_stream_set_active` from a
  foreign thread fails EPERM ("called from wrong context") and *silently*
  left the stream state unchanged — activation is now marshalled under
  the main loop's control lock (`pw_loop_lock` /
  `pw_main_loop_get_loop`), the discipline `pw_thread_loop_lock` gives
  PipeWire apps, and `AvPlayer` mirrors `sink.active()` rather than
  claiming an activation the server refused (refusals warn to stderr and
  rewind the transport flags).
- **Machine-level finding** (explains days of flaky "hangs" and garbage
  diagnostics): this box's glibc 2.44.9000/kernel 7.3.0-rc7 futex path
  intermittently starves a `std::mutex` waiter for **6–142 s** behind a
  10 ms holder cycle — gdb caught the process in `__lll_lock_wait` while
  the holder cycled normally; simple ping-pong benches stay clean, no RT
  pressure, no throttling, no seccomp, no sched_ext. The test's
  mutex-guarded capture vector hit it (and a `printf("%.0f",
  int64.count())` UB-format beside it manufactured the red herring). The
  fake device now captures **lock-free** (preallocated 60 s buffer +
  atomic published watermark — the production SPSC discipline); no
  production data path needed touching. Future threaded tests must not
  capture through a per-block mutex-guarded growing vector.
- **Checks**: new `av_sync_tests` (ctest, both presets): four layers —
  clock transport primitives (hold exact under delivered-frame and
  wall-time growth, resume ±20 ms, rebase lands also while paused,
  stream-time path obeys the same transport); real-chain feeder seek
  (feeder → mixer → paced fake device over a loud/silence/tone pattern
  wav: post-seek capture RMS 0.0000 at the silence target, 0.354 at the
  tone target, seeks and flushes counted, dead-thread wait returns false
  < 250 ms); `AvPlayer` on the monotonic fallback (silent-start, volume
  pass-through, transport flags honest); live transport (server pulling,
  clock advancing with consumption, underruns ≪ 0.5 s, pause holds
  ±2 ms, seamless resume without a jump, seek rebase + flush answered,
  paused-seek flush landing on resume; loop-off proof is the free-running
  feeder reaching eof). Live assertions matched to the real slice
  cadence (~200 ms slices on the 24000-frame sink ring). **60 checks
  green, 5×5 back-to-back runs.** Gates: desktop **25/25**, headless
  **16/16**.
- **Live reruns against the acceptance numbers**: 60 s
  `--av-bench av-10s.mp4 --preview`: presents=1501 **missed 1 (0.07% <
  1%)**, drops 0, drift mean **+0.6 ms** max 37.0 ms (**< 50 ms**), sink
  underruns **0** (235 callbacks), mixer starved **0** (was 201); 30 s
  `--second-audio probe.wav`: missed 1/751, underruns 0, both inputs
  starved=0; `--pw-check`: underruns 0, clock-vs-wall drift −18.8 ms.
  300 s soak (`--av-bench av-10s.mp4 --preview --seconds 300`):
  presents=7501 **missed 1 (0.013%)**, drops 0, drift mean **+0.6 ms**
  max 37.3 ms, sink underruns **0** (1172 callbacks), starved **0**
  across 30 loops.
- **Next**: task **5.5** — preload, continuous backgrounds, error
  handling (lyrics preserve playback; failures are visible).

## Task 5.5 log (2026-10-25) — preload, continuous backgrounds, visible errors: DONE

- **Goal**: PLAN 5.5 — acceptance "**lyrics preserve playback; failures
  are visible**": a playing background video must survive a lyric
  change, prepared slides must reach the air fast, and a broken media
  asset must announce itself rather than degrade silently. Worked as
  5.5a–e (each committed green): scene kind `c486ebc`, session
  transport `212a0a0`, preload `b633995`, visibility `6a3c8cc`,
  acceptance test + this fix.
- **5.5a — kind** (`scene/`): `Background::isVideo`, decided at compile
  time from the extension (`assetPathIsVideo`, case-insensitive), so the
  scene model stays filesystem-independent exactly like the image
  background; wire `background.video` always written, optional on read
  (legacy scene-1 payloads read as stills), refusals for a non-bool and
  for video without `image:true`; the kind participates in equality
  because the transport's continuity identity (assetId, path, kind)
  depends on it.
- **5.5b — the transport in `OutputSession`**: a presented scene whose
  background names a video gets the same `PlaybackPipeline` + `AvPlayer`
  pair the bench drives (loops always, copying-decoder fallback allowed,
  embedded-audio clock when decodable else monotonic, colour-fill
  degrade with a stderr line). The continuity rule is *unchanged means
  untouched*: `applyBackgroundTransport` compares the wanted background
  against the identity in service, so a lyric change disturbs nothing —
  decode position, clock and sink keep rolling while only the words
  crossfade. A transition involving video **cuts the media track** (the
  text track still fades — a half-composed background is worse than a
  cut); `AvPlayer` became re-armable (`stop()` re-arms, `attachPipeline`
  dedupes, new `detachPipeline`). Draw: `FrameContext` offers the held
  frame to the background layer, cover-fitted; the 1.5e dev bench keeps
  its whole-output overlay bit-for-bit (`Options::backgroundMedia` is
  set only by `--socket`).
  - **Live deadlock found and fixed**: the transport's teardown `join()`s
    pipeline threads that may themselves block on `graphicsQueueMutex`,
    which `frame()` holds — applying a scene change inside `frame()`
    therefore hangs on the first video→video swap. `applyPendingSceneChange`
    moved to the run loop directly ahead of `frame()`, outside the queue
    lock; `close()` tears the transport down before every GPU resource
    it references.
- **5.5c — preload at `scene.prepare`** (RPC thread, outside the store
  lock, socket mode only): stills decode into `SceneImageCache`, now
  thread-safe (lookup/insert under a mutex, `SkCodec` decode outside
  it — a prefetch never stalls the render thread's draw lookup; first
  insert wins; failures stay cached so a bad path is one attempt);
  videos get the cheap `probeMedia`, so a file that cannot demux is
  known *while the old scene is still on air*. Live: 4K JPEG prepare
  costs **43 ms** (the decode), identical re-prepare **0 ms**.
- **5.5d — visibility**: `output.status` gained `media {playing, path,
  error, position}`; the error is **sticky** until a successful open
  replaces it (the 500 ms poll cannot miss a failure landing in
  between), and every *new* message additionally rides a `media.error`
  notification (protocol minor **3**, capability `media.error.v1`, six
  capabilities total), flushed within one poll tick like
  `preview.ready`; an identical repeat never re-notifies (re-preparing a
  broken asset does not spam the socket). Session events
  `mediaPlaying`/`mediaStopped`/`mediaFailed`; colour and still scenes
  idle the media status (a stale video error is not the story of the
  scene now on air). Frontend: `mediaPlaying`/`mediaError` properties
  fed by status *and* notification, cleared on disconnect; the top bar
  shows the failure in amber ahead of the connection line.
- **5.5e — `background_media_tests`** (desktop preset, TIMEOUT 240,
  real `--socket` session, self-generated **silent** ffmpeg fixtures —
  a ctest never emits audio; skip 77 without the CLI): the acceptance
  shot is `media.position` (the transport's loop-continuous pts, new in
  the status object, sampled by the render thread each frame)
  **climbing through a lyric change** over the same background — a
  reopened transport would sit near zero; the background layer off
  hides the picture while the position keeps advancing; a video swap
  takes over with its own position; missing and corrupt backgrounds
  each push exactly-matching `media.error` + sticky status and recovery
  clears both; the big-still prepare is a decode then a cache hit; and
  15 preloaded slide changes time present→on-air: **p95 18.7 ms
  (max 18.7 ms) against the 100 ms target.**
  - **Real scheduler bug caught by the test**: 5.5b's move of the scene
    apply out of `frame()` broke the 1.5e capture invariant — `frame()`
    still read `presentedFor = fRevision` *after* the apply, so a
    command landing during the apply was credited to a frame that never
    drew it: back-to-back presents (the test presents the instant
    `active` flips) stalled on air with the change stuck `pending`. The
    revision is now captured in the run loop's decision critical
    section, ahead of every apply, so a presented frame can only ever
    claim commands it included (later commands re-dirty the loop —
    conservative duplicates, never a swallow).
- **Checks**: `background_media_tests` green 4× back-to-back (~35 s
  each); live smoke (`/tmp/opencode/bg_smoke.py`, kept as the manual
  harness) re-proves all scenarios with `position` visible
  (`playing=true … position: 1.12`) and an error-free worker log.
  Gates: desktop **26/26**, headless **16/16**.
- **Next**: **M6 — reliability and installable alpha** (PLAN 6.x),
  after tagging the **M5** checkpoint.

## Verification and environment

- 2026-10-23, task 1.4: `ctest --preset desktop` passes **8 of 8** —
  `worker_rpc_lifecycle` now runs four scenarios (served session,
  disconnect, display selection, **preview stream**: start 320x240 with
  geometry echo, mmap by notified name, header and size agreement, two
  seqlock-intact non-blank copies, content follows the images-layer
  toggle at >5% of pixels, all three slots rotate, stalled consumer:
  drops grow / `published` frozen / output keeps presenting, releases
  resume publishing, stop unlinks the segment, restart under a fresh
  name at the documented 640x360 default, clean exit).
  `ctest --preset headless` passes **6 of 6**, `worker_is_qt_free`
  included — `PreviewStream` and `preview_wire.h` pull in no Qt.
  `protocol_tests` gained `testNotification` and `testPreviewWire`
  (id-less envelope; segment/slot sizes, offset math, RGBA default) and
  the capability count checks moved 3→4. Seen acceptance: preview pane
  streaming the live scene ("preview · 77 frames shown · 0 dropped")
  plus `/tmp/opencode/preview-frame.png` dumped from the shm slot
  itself.
- 2026-10-23, task 1.3: `ctest --preset desktop` passes **8 of 8** —
  `worker_rpc_lifecycle` now runs three scenarios (served session,
  disconnect, display selection: enumerate, refuse index 99 and a missing
  param, arrive monitor 0, arrive back windowed, clean exit).
  `ctest --preset headless` passes **6 of 6**, and `--show-only` now
  genuinely confirms the `display` exclusion (7 registered, 6 counted)
  after the preset key was corrected to `filter.exclude.label`. Socket
  mode with no `--monitor` auto-lands on panel 1 (`output fullscreen on
  monitor 1 (DP-4)` in the log). Seen acceptance:
  `/tmp/opencode/switch-sheet.png` (DP-4 → eDP-1 → 1280x720 windowed →
  DP-4, driven only over the socket by `/tmp/opencode/switch_walk.py`,
  worker exited 0) and `/tmp/opencode/picker3-edp.png` (UI status green,
  live frame counter naming DP-4, destination picker showing the applied
  panel). Interactive drag-resize remains a manual check.
- 2026-10-23, task 1.2: `ctest --preset desktop` passes **8 of 8** — the
  seven task 1.1 tests plus `worker_rpc_lifecycle`
  (`tests/rpc_ipc_tests.cpp`, LABELS `display`, TIMEOUT 90).
  `ctest --preset headless` passes **6 of 6**; `ctest --preset headless
  --show-only` confirms the `display` label exclusion so the new test,
  which opens a window, is not counted against the no-Qt preset.
  *(Corrected during 1.3: that `--show-only` run counted 6 only because
  the test was not yet registered in the headless build dir; the filter
  key as then written was silently ignored. See the 1.3 entry above for
  the fixed filter.)*
  `worker_is_qt_free` still passes with the RPC code linked in: the worker
  speaks `poll()` and Boost.JSON, never Qt. Manual acceptance ran
  `OPENSHOW_WORKER_EXTRA_ARGS="--monitor 1" ./build/src/ui/openshow-ui`
  (UI on eDP-1, audience output on DP-4) and `--socket` with the wire
  driver `/tmp/opencode/live_toggle.py`.
- 2026-10-22, task 1.1: `ctest --preset desktop` passes **7 of 7** —
  `domain_tests`, `protocol_tests`, `worker_is_qt_free`, `ui_links_qt`,
  `worker_check_deps`, `render_scene_snapshot`, `render_layer_composition`.
  `ctest --preset headless` passes **6 of 6** with the UI off.
  `openshow-worker --render-test --monitor 0 --seconds 14` logged: GLFW
  platform wayland → fullscreen on monitor 0 (eDP-1) 2256x1504@60Hz → device
  "AMD Radeon 890M Graphics (RADV STRIX1)" → swapchain created →
  "presented 277 frames", exit 0. Windowed mode configured 1280x720 logical
  → 1728x972 buffers. Evidence PNGs (snapshot renders) in `/tmp/opencode/`.
- 2026-10-22, environment incident: KDE session 1 `LockedHint=yes` with
  `kscreenlocker_greet` running from ~10:00. While locked,
  `spectacle -b -n -f` returns the last pre-lock frame (the captured clock
  stays at 10:00 while the wall clock advances), and DP-4 was unplugged
  mid-session so `--list-monitors` reports one display. Nothing was visually
  verified during the lock; the locker was deliberately not bypassed.
- `cmake --preset desktop` configures without warnings using CMake 4.3.0,
  Qt 6.11.2, Boost 1.90.0, GLFW 3.4.0, PipeWire 1.6.9, libavcodec 63.1.102 and
  `../skia/out/Release/libskia.a`.
- `cmake --build --preset desktop` completes all 42 steps.
- `ctest --preset desktop` passes 5 of 5: `domain_tests`, `protocol_tests`,
  `worker_is_qt_free`, `ui_links_qt`, `worker_check_deps`.
- `cmake --preset headless`, then its build and test presets pass 4 of 4 with
  `OPENSHOW_BUILD_UI=OFF`, so the worker side genuinely needs no Qt.
- `./build/src/worker/openshow-worker --check-deps` prints GLFW 3.4.0, Vulkan
  header 313, Skia linked with a 2048 KiB font cache limit, PipeWire 1.6.9,
  libavcodec and libavformat 63.1.102, libswscale 10.1.102, libswresample
  7.1.102 and the offer `{"application":"openshow-worker","protocol":[0,1]}`.
- `./build/src/ui/openshow-ui` stays running under `QT_QPA_PLATFORM=offscreen`
  and `minimal` with no QML errors, so the module loads. It has not been seen on
  a physical display and nothing has been drawn with Skia yet.
- Two gotchas found. FFmpeg headers carry no `extern "C"` guards, so C++ callers
  must wrap them. Boost.JSON keeps small integers in its signed kind, so a wire
  number needs `is_uint64()` before `as_uint64()`.
- Git: origin `git@github.com:dattrax/OpenShowQt.git`, `main` in sync.
- FreeShow 1.6.7 reference clone `/home/jim/FreeShow` at
  `436d4b634878ba723da1d1373637384ec700ce16`, unchanged, with no installed
  node_modules.
- Every citation path in `docs/reference.md` was checked to exist in the clone:
  69 unique paths, no misses after fixing six wrong prefixes such as
  `EM/output/clear.ts` which is really `src/frontend/components/output/clear.ts`.
  Sampled line numbers were read back and agree, for example the 1920x1080
  `DEFAULT_BOUNDS` at `FS/components/helpers/output.ts:858` and the background
  mount delay `duration / 4 + 20` at
  `FS/components/output/layers/Background.svelte:88`.
- Runtime check of the same document: `third_party/cache/FreeShow-1.6.7-x86_64.AppImage`
  is 165640234 bytes with sha512 base64
  `MsURzL48zjECRkTTvHnmmtF0fu8QO97PTpFL12kayUw/VGdBrAyUMUh5O3qODa7yYhmG1q9bn4lZ3Q+lQ1GyOQ==`,
  equal to the published `latest-linux.yml`. Run with
  `--remote-debugging-port=9222 --user-data-dir=/tmp/opencode/fs/userdata` it
  reports Chrome/138 and Electron/37.10.3. Geometry, colours, DOM structure,
  keyboard gating, persisted settings and timer behaviour were read from the live
  pages; the method and its traps are recorded in `docs/reference.md` §12, and the
  18 PNG captures sit in `third_party/cache/freeshow-visuals/` (Git-ignored). The
  app was killed afterwards and the `Documents/FreeShow` data root it created
  during the run was removed, leaving the machine as found. The driver used for
  this is committed as `tools/cdp-inspect.mjs` and parses cleanly; it is a
  development tool and nothing in the build references it.

## Decisions

- (2026-10-28, task 6.17c3) A second colour belongs to the style, the wire
  makes it the ONE optional key, and one shader factory owns every fill.
  **The gradient is a STYLE field, not a slide field.** `backgroundColor`
  has lived on the style since the first service file, so the colour that
  grades from it lives beside it; the fit went the other way in 6.17c1
  because a fit describes how a PICTURE sits, and the picture is the
  slide's. Two related choices sitting in two places is only defensible
  when each sits with the thing it describes, and the tests say so out
  loud. `createStyle`/`updateStyle` gained the parameter WITHOUT a default
  for the same reason the fit's setter never got a convenience overload:
  a default would let a caller say nothing, and saying nothing about a
  gradient means flattening one.
  **The writer always writes the key; the reader never defaults a lie.**
  `""` is not an absent value, it is the answer "flat", so format 5 emplaces
  it on every style — re-saves are byte-stable and a format-4 file's
  re-save PROMOTES its flat into the text instead of leaving the meaning of
  the background to whatever the reader guesses. The two lies are refused
  naming themselves: a format-5 style with no key (the writer always writes
  it, so its absence is a hand-edit) and a key hand-bumped into a pre-5
  file (somebody else's build meant something by it — the format-2 `"kind"`
  rule again). What the reader does NOT do is colour-check the string:
  `backgroundColor` beside it never was, and a service that refused
  unparseable colours at read time would be a second, drift-prone parser.
  The compile owns that refusal once, naming the style.
  **`"gradient"` is the wire's one optional key, and the closed key set is
  what makes optionality honest.** Every other scene key is mandatory
  because a payload that leaves it out would leave the reader guessing at
  content the audience must see. This one is absent-or-present because
  ABSENCE has a history: every payload written before the key drew flat,
  so reading absent as flat is a fact about the bytes, not an inference.
  The other direction is covered by the set staying CLOSED — a worker
  older than minor 13 rejects a payload carrying `gradient` by name rather
  than painting the operator's gradient flat. That asymmetry (additive for
  a new reader, loudly refusing for an old one) is why the scene format did
  not move and only the capability tells a frontend when it may send one.
  **One factory, five sites, one ramp.** `backgroundFillShader()` returns
  the flat `SkShaders::Color` a flat scene always drew or one vertical
  `LinearGradient`, and it replaced the colour-only draw, the fade's
  letterbox bars, the fade's dest under-layer and the steady fill behind a
  picture. Fill sites are where a composition feature goes to become
  inconsistent: a bar filled with `background.color` beside a canvas
  showing a ramp is the bug the fixtures exist to catch (m05/m06/m07 are
  exactly those three, all caught). Same reasoning as the axis in authored
  canvas units riding the CTM: the ramp belongs to the slide, not to the
  window's pixel height.
  **The ramp interpolates gamma-ENCODED sRGB — the browser's rule — and the
  fixture pins it in bytes.** `SkGradient::Interpolation::ColorSpace::kSRGB`
  makes the midline the byte mean of the pair, which is what a browser
  drawing the same two colours produces; linear-light would put the same
  pair's midpoint at R165/G107 against the encoded mean's R120/G88, 45
  units of red of daylight. Choosing an interpolation because it matches
  the reference's look is a doctrine; a midpoint spelled out as `#785838`
  with a 2-unit tolerance is what makes it enforceable, and m04 (switching
  to `kSRGBLinear`) dies on it.
  **Equality gates the second colour on its flag.** `flat.gradient = 0x123456`
  and `flat.gradient = 0x654321` draw the same canvas, so treating them as
  different scenes would make the worker re-present a picture nobody can
  distinguish; `hasGradient` is what means something, exactly as it is at
  the draw. Flat-under-gradient is the opposite case and also asserted:
  that is a new picture.

- (2026-10-28, task 6.17c2) Blur is an honest name widening, the backdrop
  is derived image data, and the video pairing is refused at every gate
  that can see it.
  **No format number moved.** c1's closed-name refusals are the promise
  that lets a name join later without ceremony: a build that does not know
  `"blur"` REJECTS the file or payload naming it, so widening the set is
  safe for it and additive for new builds. What a frontend must check is
  not the format number (which lies about nothing here) but the
  capability — `scene.background-fit-blur.v1` — before offering the combo
  entry to a live worker.
  **The backdrop lives inside the image's cache entry, not beside it.**
  It is pixels derived from those bytes: its bytes join `fBytes` (evict,
  invalidate and install-over each subtract what they added — a derived
  image escaping the budget would make `stats()` a lie), it is memoised
  per path, and it dies with the image. The build runs off-lock at the
  first blurred draw: measured **83 ms** at 1920x1080, i.e. one frame's
  work at the slide change, never in the frame loop — R12's "the draw
  never decodes" generalises to "the draw never blurs". The downscale-
  first approximation was MEASURED against the by-the-book build, as the
  plan required: **20 ms vs 83 ms, worst-channel difference 3/255** on
  the banded fixture — and the full-resolution build still ships, because
  the speed was never the problem the composition budget had.
  **Blur's alpha is baked into the backdrop (77 ≈ 0.3), not carried on a
  paint.** Every path that draws one — steady, the fade group's two
  regions, any companion renderer later — shows the reference's opacity
  without remembering to set it, which is the same reasoning that bakes
  the overscan into the geometry: a constant a caller must remember is a
  constant a caller will forget.
  **The video pairing is refused at three gates** — the editor's seam,
  the compile, the wire — rather than once. A rule about the draw budget,
  stated only at draw time, surfaces as a misdrawn frame; stated at every
  gate that can see both halves, it surfaces as a sentence an operator
  reads where they made the choice. The duplication is deliberate and the
  tests pin each gate's own refusal, so no gate's absence can hide behind
  another's.
  **The fade nests its shaders instead of getting a layer** —
  `Blend(picture, Blend(backdrop, fill))` — and c2's battery proved the
  fixture lesson: an opaque `SrcOver` picture ignores its dst shader, so
  the nesting is INVISIBLE to an opaque fixture at any blend (m14
  survived a full run). The promise only exists through a translucent
  picture, where the backdrop under it reads ~12/255 at blend 0.75 — the
  fixture the mutant now dies against. Opacity in a fixture is a claim
  about what the test can see.
- (2026-10-28, task 6.17c1) A fit belongs to the slide, the wire demands it,
  and a name nobody can draw is refused rather than approximated.
  **The choice lives on `Slide`, not on the asset.** The same picture under
  two slides can crop on one and letterbox on the other — the reference
  agrees (its fits are per-slide style), and an asset-side fit would make
  importing one file a decision about every slide that ever shows it. A
  slide with no picture still carries its answer, so picking contain before
  the photo arrives is legal and survives; the worker reads the field only
  when there is something to fit.
  **`background.fit` is MANDATORY on the wire, and the format number moved
  with it (2→3, no tolerance either direction).** The gentle version —
  absent means cover — was rejected for the reason 6.17a/b rejected every
  defaulting reader: a payload that omits a key forces the reader to guess
  what the writer left out, and the guess becomes the audience's picture.
  The service FILE defaults (formats 1/2/3 load as cover) are the opposite
  case and were kept: those files are this build's own history, cover is
  the only thing their era could draw, and their re-save promotes the
  choice into a fact. The asymmetry is deliberate — a file you wrote is
  history; a message someone is sending now either says what it means or
  is refused.
  **The refusal of unknown names is what makes the 6.17c SPLIT safe.** c2
  will add `"blur"`; until it exists, a build that meets the name in a file
  or a scene rejects it quoting the name (`not a fit this build can draw`).
  Had c1 defaulted the unknown, an old worker would have silently drawn a
  blur-requested slide as cover and the file's meaning would have depended
  on which binary opened it — the definition of the lie this project
  refuses everywhere else. The same rule at the controller (a refused name
  never reaches the edit seam) buys a provable claim: one undo after a
  refusal still reverts the last real fit, because a refusal pushed nothing.
  **The fade group's fix cost 92/255, and the bug was three milestones
  old.** R13's composite is one `picture over fill` shader; drawing it over
  the whole canvas with the image's CLAMP tile mode extends the picture's
  edge pixels past its dest rect. Cover's dest always contained the canvas,
  so no fixture could see the clamp — until contain put bars outside it,
  and the first contain fade measured 92/255 in the bar rows: clamp-fed
  picture where the fill belongs. The fix keeps one composite per region
  (bars: the fill at the group alpha through a clip-difference; picture:
  the same shader on its rect, disjoint draws, no layer, no render target)
  and measures 0/255 across the whole frame against the steady-frame lerp.
  The general lesson is written where it can be found: a fixture that can
  only sample inside the interesting geometry is a fixture that will never
  see the geometry change.

- (2026-10-28, task 6.17b2) The panel places; the model refuses; and the
  field that cannot hold the words is not shown.
  **Placement is `addTextBox` for pictures, and its refusals are borrowed,
  not restated.** The temptation at a new door is a fresh validation layer —
  check the asset exists, check it is not a video, check the count — written
  at the controller. Every one of those is `addItem`'s rule already, owned
  there since 6.17b1 with worded refusals, and a panel-side copy is a second
  list that can drift from the first (the exact failure class that made
  `assetPathLooksLikeVideo` one list last leaf). So `placeAssetOnSlide`
  validates only what only it knows — that there IS a current slide — and
  quotes the domain's own error for everything else. What it adds is the
  flush it opens with: the one rule with no model-side equivalent, because
  "whose words are still in the debounce" is session state. Without it a
  placement silently converts a pending burst into a refused one — the
  operator loses typed words to a guard that was correct about the picture
  and irrelevant to the sentence they were writing.
  **Honest refusal needs a reader.** The model has refused words on a picture
  since 6.17b1, and the controller's `edit()` made that refusal INVISIBLE:
  the burst vanished with no trace, which is the worst shape a correct
  refusal can take. So `commitPendingText` names it on the status line — and
  the QML does the kinder half, hiding the words field on an image selection
  so the refusal is the backstop rather than the routine. The battery proved
  the distinction matters: with the setStatus deleted, the old
  `contains("picture")` check still passed on the placement's own success
  status, and the check had to move to the guard's own phrase.
  **A default box is a statement.** Placing a picture on the lyric default
  (160,100,1600,880) puts its outline exactly over the text box's and the
  canvas says "nothing new" — while a centred half-canvas arrives as a visibly
  new box on the composition, large enough to judge the picture in, and the
  drag the operator was always going to do is one gesture away. The number is
  pinned by test so the next default is a decision, not a drift.
- (2026-10-27, task 6.17b1) Kind is a fact, absence is a fact, and neither is
  allowed to be inferred.
  **A stack entry says what it is.** The cheap version of this leaf inferred the
  kind — `assetId` non-empty means picture, empty `lines` means picture — and
  every one of those readings is wrong the first time a slide has an empty text
  box or an item someone hand-edited. So `kind` is a field, written to the file
  for TEXT items too (a default that changes would silently redefine what an
  old file drew, which is the one thing a format number exists to prevent), and
  the scene wire's closed key set became closed PER KIND: typography attached to
  a picture, or a path attached to a verse, is the same silent-gap class the
  scene format has refused since 6.8a. Format 3 accepts no `"kind"` in a
  format-2 file for the same reason: the key did not exist there, so its
  presence means another build meant something by it, and guessing is the quiet
  corruption the loader is supposed to make loud.
  **One video test, because two lists drift.** The model refuses a video asset
  in the foreground and the scene library decides whether a background is a
  video. Those are the same question — "does the media pipeline read this name
  as moving pictures?" — so `domain::assetPathLooksLikeVideo` is the one name
  list and `scene::assetPathIsVideo` delegates. A fixture reaches the
  compiler's guard through the real path (task 6.12's adoption rebinding a
  placed picture onto `.mp4`) rather than trusting the two agree.
  **A picture without its file is not a half-drawn item.** Deleting an asset
  degrades a BACKGROUND to none, because "no backdrop" is a slide. It cannot do
  the same to a stack entry: an item is a position in a composition, and an
  item naming nothing is a claim about the picture the worker would have to
  invent. So `removeAsset` drops the entries, and the reason that is SAFE — not
  data loss — is that History holds whole states: the undo puts the asset back
  AND its items, each with its own id in its own slot. The same reasoning
  decided `setSlideLines`: the legacy single-text view addresses item 0, item 0
  can now be a picture, and the two lies available (type into the image, or
  invent a box under it) are both worse than the refusal.
  **The media lane stayed background-only, on purpose.** A foreground decode
  failure is a stderr line while the old scene is still on air; it does NOT
  touch `status.media` or push `media.error`. `status.media` names what the
  audience's transport is showing, and a stack picture nobody is playing would
  evict that truth with a file. The honest on-screen answer is already correct —
  the box shows what is under it, which is visibly not the slide the operator
  composed — and per-item fault reporting belongs to R19's media lane, where
  there is something to report INTO. Disclosed as unmutated rather than
  papered over.
  **A row's unit of debt is the ROW; its unit of work is a path.** 6.16b's
  thumbnail pump settled one path per job, which was invisible while a slide
  named one file. A slide with a backdrop and a picture now carries a path
  VECTOR plus settled flags, draws when the LAST one settles, and keeps the
  lookahead counting finished jobs so the memory bound means what it meant
  before. The check that pins it is a row whose two halves are different colours:
  measured r134 g142 b142 against the first-settle-draws mutant's r239 g222
  b46 — pure backdrop, the frame shipped before the second decode landed.
  Then the battery's own lesson, second time: two mutants were scored against
  anchors that had gone stale (a re-indent, a dropped `.empty()`), so the dry
  run now preflights EVERY anchor before a run. A mutant that cannot run is the
  driver lying, not a fixture passing.

- (2026-10-27, task 6.17a2) Where a fact lives, and three ways a fixture
  nearly lied on the way.
  **Selection is a fact, so it gets one home**: the obvious way to add box
  selection was a `property int selectedBox` in EditCanvas.qml — but that
  answer would live in one panel while the inspector, the fields, the nudge
  keys and the worker ask live in four other places, and "which box is the
  session editing" would have five readings that drift the first time any of
  them learns about a second kind of edit. The selection moved to
  `DocumentController` as an ID with a documented fallback, and — the part
  that actually prevents drift — every reader, display or edit, resolves it
  through the SAME function (`selectedBox()`), so a highlighted box and an
  edited box cannot disagree even in principle. Selection dirties nothing (a
  click is not an edit, and autosaving one would make the crash net lie about
  what the operator changed), but it flushes pending keystrokes first:
  debounced text belongs to the box it was typed into.
  **Undo is a time machine for identity, and the selection should ride it**:
  deleting a box deliberately LEAVES the selected id pointing at nothing —
  the fallback shows box 0 meanwhile — so an undo, which restores the exact
  snapshot including ids, puts the selection back on the very box the
  operator deleted. Navigation instead CLEARS the id: the selection belongs
  to a slide, and a carried id that silently re-selects an old box on the way
  back is a trick the operator never asked for. And a gesture detail the code
  nearly got wrong: a corner handle of the selected box can lie over another
  box, so a press that re-selects by topmost hit would anchor the resize to
  the old box and commit onto the new one — body presses select, handle
  presses never do.
  Then the three nearly-lies, all found by the mutation battery rather than
  by an operator. **A check that passes when the bug is present is not a
  check**: "navigation clears the selection" originally selected nothing
  before crossing slides, and an id-overwriting add sat between the crossings
  — a controller that carried the id would still fall back to box 0 and pass.
  **A comparison against a dirty baseline proves nothing**: the dirty-free
  click was measured against a second controller on the same directory, which
  RECOVERED the autosave scratch and was dirty before the click; "still dirty
  after" was true of any controller, chrome or not. It now runs on a pristine
  copy of the service where nothing was ever autosaved. **A mutant scored
  against a suite that does not contain its code is the driver lying**: the
  first m16 mutated `compileSlide` while running the live preview suite
  alone, whose legs hand the worker WIRE scenes and never call the compiler —
  it "survived" for a reason that belonged to neither the fixture nor the
  product. Retargeted at the renderer (where the live leg sees it: the
  stacked frame goes mean-identical, 2.593368 → 2.593368) and with the
  compiler scored where the payload checks live, all four re-scored to named
  failures. One rule kept from earlier and applied again before it bit: every
  mutant's rebuild set includes whatever binary a live fixture SPAWNS, or the
  mutant is scored against a build that never saw it.
- (2026-10-27, task 6.17a1) Four rules about identity, and three about the ways
  this repo's own testing nearly lied.
  **A model with two representations of one fact is two chances to disagree**:
  the cheap version of this change kept `Slide::lines` as "the main box" and put
  `items` beside it. It compiles, and then every reader picks a favourite and
  every writer has to remember both, and the first thing to drift is a preview
  an operator trusts. The legacy pair is now a documented VIEW onto
  `items.front()` — the box a pre-6.17a slide has exactly one of — and a slide
  the operator drained to nothing REFUSES the legacy ops naming itself rather
  than growing a box back under their hands.
  **A format version exists for the reader who is not in this build**: adding
  `"items"` as an optional key to format 1 was the tempting move, and it is the
  one that hides the bug — an older build opens the file, finds no slide-level
  `lines`, shows one box of a slide whose author composed four, and says
  nothing. Bumping to 2 makes that build refuse the package loudly; format 1
  still loads here, migrated. The corollary: an identity a load invents must be
  a FUNCTION of the file (`slide.id + "-text1"`, derived and deterministic,
  promoted into the file by the next save). An id minted from a counter agrees
  with itself only until the same file is opened a second time, and then the
  operator's selection is pointing at a different box than it was.
  **Two libraries, one number, pinned by a test**: `kMaxTextBoxes` is
  `scene::kMaxSceneItems`, and neither header may include the other's, so
  `exerciseBoxConstants()` compares them. The guard inside `compileSlide` that
  the equality makes unreachable stays — the libraries are compiled separately,
  and a scene this function emits but its own codec refuses is a present-time
  failure — but it is DISCLOSED as unmutable rather than counted in the tally.
  **A copy is a copy, or it is a new thing**: the clip clears the ids it copies,
  `addTextBox` drops the id it is handed, a slide pasted twice gets two sets of
  identities, and pasting a slide with NO boxes removes the default box the new
  slide arrived with. A copy that quietly adds a box is a different slide, and
  the operator finds out by pressing undo once and having two boxes appear.
  Then the three testing lessons. **A C++ raw string ends at the first `)"`, so
  a JSON fixture needs its own delimiter** — two hand-written manifests were
  malformed in the source and got refused for a reason that had nothing to do
  with what they claimed to test, and the surviving fixture says so where it
  sits. **A tamper anchor that stops matching is a test that passes by not
  testing**: the "future format" case had been replacing `"format":1` since
  before the format moved to 2 and passed both ways; it now parses whatever
  number the writer emitted and adds 1000, and the helper still reports failure
  when it finds no anchor. **A refusal assertion must print what the refusal
  actually said**, or a wrong expected string gets "fixed" to whatever the code
  happens to say; the box-refusal helper now prints the loader's sentence on
  mismatch, which is how a wrong expectation was found in the test rather than a
  bug in the loader. One more, inherited and applied here before it bit: the
  mutation battery's rebuild set includes the WORKER binary, because a live
  fixture that spawns a stale binary scores a mutant that never ran.

- (2026-10-27, task 6.16b) Four rules, one of them about how this repo tests.
  **A shared helper that ignores what makes its callers different is worse than
  two helpers**: `fillPreviewSlot` had been given the renderer and the surface
  to draw with and used `fScene` and `fPreviewSurface` regardless, so the
  slide list's rows came back the scene's fill colour (the show's image cache
  had never heard of the backdrop the list had decoded) and every row
  alternation reallocated the audience's mirror surface. Parameters that are
  not used are not a style problem, they are a second, silent behaviour — and
  the only thing that caught it was a fixture that asks whether the row's
  pixels are the row's own backdrop.
  **QML is not compiled, so run the binary**: `PreviewView::key` was an
  accessor pair with no `Q_PROPERTY`, and three `root.` references pointed at
  the Window rather than the `showPage` Item that owns the band's function and
  flag. Both compile, both install, and both leave the operator an empty
  window; the standing answer is the new `ui_loads` ctest, which requires the
  real executable to stay up offscreen and to have said nothing about loading,
  with negatives (a process that exits at once, and the actual output these
  rows produced while they were broken) so the guard cannot go vacuous.
  **A status field means what the client's recovery rule assumes it means**:
  the scripted peer reported the thumbnail target's `active` as its remaining
  row debt, and the client — which re-asks when the worker says the band is
  gone while a list still stands — re-asked one completed band four times a
  second. `active` is registration; `queued` is the debt; the first run of the
  scenario is the comment in the peer now.
  **A bound you do not enforce at the producer is a bound you do not have**:
  `decodeAtMost` was satisfied by a decode that respected the box "where
  possible", and PNGs that came back full-size sat in the pool at 33 MiB for a
  160x90 row. `decodeImageFile` now resamples whatever is still over the box
  after `RasterFromBitmap`, and the fixture measures the cached entry's bytes.
  And on the battery: **when two guards protect one property, mutate the pair
  and disclose the half**. The batch cap and the key limit are checked at the
  wire and again in the session, and the extent is checked at accept and again
  at publish; removing either half alone changes no observable, so m08 and m10
  remove both (caught) while m08a, m10a and m04a are disclosed as equivalents
  rather than counted as wins. Leg 9 of the live fixture exists for the same
  reason: it stands a `480x480` band across a `320x180` `preview.start`, which
  is the only input that makes the pair's absence anything other than a
  status-field difference — with both guards gone the worker dies on the
  oversized write, which is precisely what they are there to prevent.
  Two more tallies belong with that rule rather than being smoothed over.
  **m19 (the capability removed) first reported "survived" and was a harness
  bug, not a result**: the protocol library had been left out of the battery's
  rebuild set, so it scored a mutant against a binary that had never been
  compiled; after fixing the driver and adding a preflight that checks every
  suite entry's shape and that each named executable exists, the re-score was
  CAUGHT. A mutant that cannot run is a driver lying. And **m20, the client's
  id-map gate, is disclosed as equivalent rather than decorated**: a frame whose
  id no accepted batch handed out is copied and then refused by the store step
  for want of a key, so no observable moves — the gate bounds per-frame copy
  work, which is real but not visible to a fixture, the same class of truth as
  6.14b's pause-drop. Final: 19/23 caught, four disclosed.

- (2026-10-27, task 6.16a) Three rules, plus an autopsy worth keeping.
  **Extend the transport you have before you add a parallel one**: the cheap
  shape for a second picture was a second shm segment, and it is worse than it
  looks — two slot accountings, two identity rules, two drop policies, and the
  interesting failure (a frame of the wrong kind arriving under perfectly valid
  handles) moves to a seam no fixture watches. Versioning the layout instead
  costs one field pair, and the rule that makes it safe is the same one 6.5
  taught me about optional fields: an ABSENT field means the old meaning, so a
  version-1 frame is the audience's picture and not an undefined target.
  **A rule that crosses the wire needs both halves, and they are not the same
  rule**: the producer refuses a generation that does not advance, the consumer
  refuses a frame answering an ask it has already superseded. The first is
  state on the accept, the second is state on the newest ask, and only the
  second one protects the operator's eye — the honest race is that the render
  thread was already drawing yesterday's edit. Which forced the rule I most
  want written down: **if a property is about a race, the fixture has to be
  able to ARRANGE that race, not hope for it**. The scripted peer therefore
  takes markers in the payload the frontend already sends (`SLOW` holds a frame
  500 ms, `FORGET` reports the target gone once) instead of the code growing
  hooks to be testable: the marker travels with the scene, the ledger the peer
  writes is verbatim, and the two markers turned the two hardest paths in R16
  into ordinary checks. Related: **when a counter is published lazily, assert
  the consequence**. Mutation m07 made an off-air draw bump the presented-frame
  counter and my "the counter stays flat" check passed, because `status.frames`
  is copied at the end of a composed pass and an idle audience composes no
  further pass — the corrupt value simply was never republished. The honest
  question was never "does a number move" but "what must not happen because an
  edit was drawn", and downstream of that counter sits the mirror's debt: an
  edit that pretended to present would leave the audience's stream owing a
  frame nothing presented, which is observable at rest. The rewritten check
  caught m07 immediately. And **know what your smoke can see**: qmlcachegen
  turns QML into C++ and swallows an invalid property assignment into a silent
  process exit, so "no error text on stderr" was worth nothing; the smoke now
  also requires the app to still be alive at its `timeout` deadline, which is
  what a load failure actually takes down. The autopsy rule: when a mutant
  survives, sort it into ONE of three bins before touching anything — a weak
  check (fix the check: m07, m20, m22), defense in depth (mutate every layer
  that holds the predicate, and if the all-layers mutant is caught say so: m04
  vs m04b), or a cost policy no fixture measures (disclose it and say why a
  check would be a stopwatch on a shared desktop: m06). Five survivors in run 1
  were three of the first, one of the second and one of the third, and each
  bin has a different honest answer.

- (2026-10-26, task 6.15b) Three rules came out of giving the standalone
  track its lifetime. **A lifetime chooses its own component**: the obvious
  cheap shape was a second mixer input on the background's `AvPlayer`, and it
  is wrong — the background transport is destroyed and rebuilt on every
  background-identity change, so a track living inside it would restart at
  precisely the moment R15 forbids. When a requirement is phrased as "must
  not restart", the object that holds the state must be one whose teardown you
  cannot trigger by accident; sharing a sink stream is a detail the server
  handles, not a reason to share a lifetime. Two consequences the fixtures
  needed: a registration table shared by two lifetimes is read-modify-write
  per bit (the background's bring-up and teardown now touch only bit0, and a
  test that never has one live without the other cannot see a clobber), and
  the per-identity gate is only testable in the moment one transport is live
  while the other is not — so the fixture removes the TRACK while the
  background plays on, which is also the half of the acceptance that reads
  "removing either source leaves the other correct". **Assert the reason, not
  just the refusal**: mutation m03 (the probe accepting an unopenable path)
  survived its first run because the very next guard rejected the same file
  for a different reason and the check only looked for the path in the
  message. Where several guards refuse one input, each leg gets its own
  witness naming its own reason — the same rule 6.15a derived for command
  gates, now with a concrete near-miss. **Know which clock a check is
  asserting on**: my first loop witness polled `position` for a wrap, and the
  suite said "skipped", because the position an audio-only transport reports
  is the HEARD sink clock, which runs on past the file's end whether the
  feeder rewound or not — a starved mixer pads silence and the server
  consumes that at the same rate. That is an observability hole, not a test
  gap: "the bed ran out" and "the bed rewound" were indistinguishable to the
  operator, so the fix was to publish the feeder's wrap count in the status
  block, not to test around it. The same episode left two smaller rules —
  a sentinel value makes a comparison meaningless (the block seeds `position`
  at -1, so a pinned 0.0 "advanced" by 1.0 and m13 survived until the check
  required a SAMPLED reading and a bounded step), and `live` plus the audio
  lane are one statement about one transport, so the swap that sets the first
  owes the second an honest value rather than leaving the last block's
  "none". Finally, disclosed rather than argued: m16, removing `close()`'s
  explicit track stop, is mutant-equivalent because `~AvPlayer()` calls
  `stop()` — the call documents intent and is not the mechanism, and the
  battery says so plainly.

- (2026-10-26, task 6.15a) Three calls shaped R15's first half.
  **Enablement is a distinct signal from asset identity**: the controls
  enable on `media.live` and never on `media.path`, because 5.5d left
  path with two honest teardown flavors — a failed asset keeps it so the
  sticky error stays paired with the file it describes, a background-less
  apply clears it because path names what the audience is looking at —
  and neither means "commandable". A fixture asserting the wrong flavor
  caught this: the teardown check had to follow each flavor, not one
  guessed rule. **Refuse NOW, name the reason, at every layer**: R19's
  swallowed op was the bug being fixed one layer up, so a command with no
  transport, or naming a source the transport never registered, is an
  error the caller hears — the session returns the string, the RPC echoes
  it, nothing queues for nobody. Corollary the fixtures pin: the client
  does NOT second-guess the worker's source table; it sends the gain even
  for a source it suspects is absent, and the refusal leaves the mirror on
  the worker's truth rather than a locally edited row. **Ack `pending`,
  confirm via status** (3.4's rule, now also for loop/gain/mute): the
  command hands an intent to the transport's thread, so the only honest
  acknowledgement is the next status echo. That choice made both test
  layers anti-optimism fixtures almost for free — the scripted peer moves
  its transport only via `media.*` and the controller's properties move
  only via status, so a control flipped by the click cannot survive, and
  the peer's verbatim ledger proves WHICH frames rode the wire. One
  measurement lesson: the loop-off final-frame hold must poll for a
  SETTLED position — decoded frames keep pacing ~2 presents past the EOF
  flag (5.88 → 5.96 → stable), so the promise is "two reads agree and
  keep agreeing", not "the first read after playing:false is final".

- (2026-10-26, task 6.14b) Three rules came out of doing R14's second
  half. **Bound the queue, not just the thread**: one running job plus one
  replaceable pending slot is enough because the crash net only needs the
  newest state — unbounded queues turn a slow disk into a memory leak, and
  "keep newest" (m10) is the property the fixtures actually test. **Order
  decisions against in-flight work structurally, then veto**: the pause
  protocol (drop pending, WAIT out the running write, act, resume) makes
  clear-after-save an ordering guarantee, and the decision epoch is belt
  and braces over jobs and acks that raced past the checkpoint — one of
  the two alone would work, and the battery honestly says only the
  combined removals are observable (the pending-drop alone is mutant-
  equivalent to the epoch veto). **A test seam is a fixture requirement,
  not a code detail**: the delay variable was dead code for a whole battery
  run until a fixture timed the wait it made possible (m05), exactly like
  the same-size replacement that hid m02 — an unmeasured guarantee is a
  hope, and both escaped mutants say the same thing about test data as
  6.14a says about error strings. The default stayed DORMANT (inline)
  rather than threaded: ten months of fixtures encode synchronous
  autosave, and opting production in with one call kept every one of them
  valid as the single-threaded control group.
- (2026-10-26, task 6.14a) Three rules generalise past this controller.
  **An error path that ends in a local variable is not a design, it is a
  hope**: five call sites believed they reported autosave failures because
  they collected the strings — R14's real finding is that a report nobody
  reads is indistinguishable from none, which is why every mutant here is
  one of those discards restored. **When two writers share a status line,
  order the writes deliberately**: the refusal defers to the next event-loop
  turn precisely so it outlasts the caller's success line — "last word"
  is a contract, and the import fixture makes it testable. **Shutdown
  hooks belong to the object that owns the state**: connecting
  `aboutToQuit` inside the controller made the flush invariant
  unforgettable and testable by the real signal rather than a hand-wired
  call in `main()` — the earlier main/QML design was dropped mid-task for
  exactly that reason. The race guard (a queued warning must check, when
  it fires, that it is still the truth) is the general form: deferred
  effects carry a veto, not just a payload.
- (2026-10-26, task 6.13b) Validation belongs at the chokepoint, and
  containment belongs at the filesystem, not the string. One gate inside
  `claimId` protects six kinds of id because every loaded id must pass
  through it to be claimed — rules written per-call-site are rules someone
  will forget. And "does this path stay inside?" has exactly one honest
  answer-source: `canonical`, comparing component by component. A string
  compare cannot know that `pkg-evil` starts with `pkg`, and a check done
  before following symlinks can be pointed anywhere; the fixture keeps a
  sibling directory named `<package>-evil` with a secret file behind a
  symlink precisely so that bypass is a CAUGHT mutant rather than a
  comment. The same principle read backwards is the feature: authorised
  import (the operator naming an external file) stays unrestricted — the
  rules bind what a FILE may claim, never what a user may choose.
- (2026-10-26, task 6.13a) The commit protocol generalises three rules past
  this package. **Never write in place a file a reader may trust**: write a
  new name, then rename — rename's atomicity is what turns "the bytes are
  somewhere" into "the name resolves to a whole version", and it makes
  process interruption a non-event for readers. **Durability is a barrier
  crossed before success is reported, and the report crosses it too**:
  fsync per file and per naming directory, with every status (write, fsync,
  close, rename) inspected — a close() whose failure nobody reads is a lie
  with a return value. The one thing that cannot roll back (the last
  directory sync after a committed rename) gets its failure written down
  rather than papered over. **A fault-injection seam is a contract test for
  error paths**: named steps that refuse on demand, read per step so
  fault → recover → re-commit runs in one process, messages naming their
  step so fixtures and operators can tell an injected refusal from real
  disk trouble. Its one blind spot — an in-process test cannot observe a
  silently-deleted fsync — is closed by external observation (the strace
  count), not by more assertions.
- (2026-10-26, task 6.12) A save that copies bytes is a MOVE of ownership,
  and ownership has to be TOLD. The defect was not the copy — correct since
  2.2 — but that every other holder of the path kept reading the old
  location: the model, whole-state history snapshots, and compiled scene
  payloads already sitting in the worker controller's caches. The rule that
  generalises: **a snapshot-based history is a hidden cache of everything the
  state references**, filesystem paths included; adopting storage moves must
  rewrite the snapshots, and the undo and redo stacks are two separate
  oversights (the redo one only bites when the operator undoes BEFORE the
  save, so the snapshot sits in `fRedo` at adoption time — fixtures need that
  order, not just the obvious one). Second rule: **rebind reports beat
  re-derivation**. The save hands out `{id, from, to}` triples and every
  holder applies the same map, matching id AND old path: a holder that was
  updated after the save keeps its newer value instead of being yanked back
  by a stale rebind, and nobody has to re-read disk to find out what moved.
  Third: **prepare, never present** — the worker needed no new primitive
  because its media pins are path-keyed; the on-air path stays decoded and
  pinned until the next present swaps to the new one, so renamed payloads can
  be re-prepared silently. Re-presenting on adoption would restart a fade the
  audience is watching for a rename they cannot perceive.
- (2026-10-26, task 6.12) A mutation that ESCAPES is a fixture bug worth
  more than ten that get caught. m12 (skip rewriting the retained restore
  snapshot) survived because S12 adopted while the restore slot was EMPTY:
  the snapshot was later taken from the pending payload, which the adoption
  had already rewritten, so the skipped rewrite was never observable. The
  general rule for testing caches: the rebind must arrive while the holder is
  ALREADY populated with the old value — clear (retain snapshot) → adopt →
  restore, not adopt → clear → restore. Fixtures that populate the holder
  after the event test inheritance, not the rewrite.
- (2026-10-26, task 6.11b) A group alpha without a group target: put the
  composite where the pixels are. Every fade wants transparency applied to a
  BLOCK as composited, and `saveLayer` buys that with a render target per frame.
  Both fades here have a target-free form because their block is one thing the
  canvas already knows how to evaluate: the background's block is `picture over
  fill`, which is a shader (`SkShaders::Blend(kSrcOver, Color, Image)`), and the
  words' block is a paragraph, which will hand every draw it makes to a
  `ParagraphPainter` the caller supplies. The general rule worth keeping: before
  allocating a target for a group effect, ask whether the group has a single
  drawable — most do, and the shader or painter answer is also cheaper (the
  layer cost 2× a settled words frame on the CPU backend; the shader group cost
  less than the wrong two draws). Two lessons from doing it. **A shader's local
  matrix is in the image's own pixel space**, the same `RectToRect(src_pixels,
  dst)` the canvas builds inside `drawImageRect`; a unit-square matrix samples
  the top-left pixel everywhere, and no flat-colour fixture can tell — the
  banded, non-canvas-shaped fixture is what caught it, so an image fixture needs
  geometry that changes, not just colour. **And attenuation composed twice is
  not attenuation once**: the bug being replaced multiplied the fill and the
  picture by the blend separately, which differs from the group by
  `t(1-t)(fill − picture)` — zero at both ends of every fade, worst in the
  middle, and invisible to a scene with no picture. Fixtures for a fade must
  therefore be measured mid-transition and against a loud second source.
- (2026-10-26, task 6.11b) When the rule is about the code, guard the code. A
  group layer put back into the words fade is PIXEL-EQUIVALENT (the parity
  fixture passes either way), so no measurement can keep `saveLayer` out of the
  composition pass — only a source scan can. `render_layer_guard` therefore
  exists with an empty exemption list for `saveLayer`, and its negative fixtures
  are what stop it scanning nothing. It also had to be shown that a comment is
  not a hiding place: m10–m13 are one violation visible, one violation hidden
  behind `/* … */` on the same line, and the scan had to catch both (it strips
  inline comments and only skips a line left with nothing but comment text). The
  general rule: a mutation battery is also a test of the GUARD, not just of the
  fixtures, and a guard that only catches the obvious spelling is not a rule.
- (2026-10-26, task 6.11a) A composition two targets read must be a VALUE, not
  a query. `SceneRenderer::draw(canvas, …, timeSeconds, …)` looked pure and was
  not: it advanced the shared crossfade, so the second target of a frame saw
  whatever the first had left, and a preview could run a fade to a blend the
  audience never saw or retire it while the panel still held a half-blend. The
  fix is a captured `SceneFrame` (scene, both blend tracks, clocks, revision)
  with exactly one mutator — `captureFrame` on the audience's clock — and
  retirement moved to `commitFrame`, the point where the frame is KNOWN to have
  reached the panel. Retiring on computation is the subtle half: the
  event-driven loop presents while `transitionRunning()`, so a blend that
  completed in a frame that then failed to present would leave the audience
  holding a half-blend nobody intends to finish. Retirement also needs the
  scene-identity guard, or a stale credited frame ends a fade a newer present
  has just started. The old draw overloads are deleted rather than deprecated:
  a target that cannot ask "what does it look like now" cannot answer with a
  different moment than its sibling — and the one form of "the same moment"
  observable from outside is directional, so the live fixture lets the preview
  LAG and refuses only a LEAD.
- (2026-10-26, task 6.11a) Sharing work between targets is not free, so the
  audience prices it. The preview's share is 25 % of the frame period and an
  expensive publish pays its overrun back in deferred attempts, which holds the
  AVERAGE inside the share whatever one frame cost; `preview.{deferred,worstMs}`
  make the trade visible instead of asking the operator to notice a stutter.
  The lesson is about deferral in general: **a refusal must move the loop's
  wake-up, not just the callee** — gate the work but leave `fLastPreview` in the
  past and the render loop asks again twenty thousand times a second (measured
  58,035 stand-downs in one 700 ms window, against 44 once the horizon feeds
  `nextAllowed`). Fixture law paid for here: bound such a meter by what the
  loop can physically do (one attempt per audience frame, plus one per horizon
  wake, plus housekeeping), never by the counter's own value, and publish the
  control run — 29 publishes, 0 deferrals, 3.14 ms worst cost — because that is
  what makes both the floor and the ceiling credible.
- (2026-10-22) Output composition follows the layer model now recorded in
  PLAN.md: one pass, no render-to-texture, disabled layers functionally
  skipped, frontend-toggled; audio is never a composition layer.
- (2026-10-22) Skia Vulkan requirements proven on this revision:
  `fMemoryAllocator` is mandatory; wrapped swapchain images need
  TRANSFER_SRC **and** TRANSFER_DST usage bits; Wayland's undefined
  `currentExtent` is handled with a framebuffer-size fallback plus
  OUT_OF_DATE recreate.
- (2026-10-22) ICU: hard-coded SkUnicode backend plus the
  `SkUnicodes::ICU::Make()` shim until an ICU-v77 match (or a rebuilt Skia)
  lands in M3; ASCII-only break rules are explicitly a task-1.1 scope limit.
- (2026-10-25, task 6.2b) Unicode is the public ICU-backed Skia module
  against the SYSTEM ICU: Skia builds with `skia_use_system_icu = true`
  (the user's call), so the worker, libskunicode_icu and Fedora's ICU share
  one version and the homemade hard-coded backend plus the
  `SkUnicodes::ICU::Make()` shim are deleted. This supersedes the 2026-10-22
  hard-coded-backend decision.
- (2026-10-25, task 6.2c) Skia is vendored as the `third_party/skia`
  submodule pinned to the reviewed revision and built by
  `scripts/build-skia.sh` (invoked automatically by CMake for a
  library-less submodule); the separately cloned sibling `../skia` becomes a
  legacy fallback selected only through `SKIA_ROOT`. Supersedes the
  build-separately layout documented up to this point.
- (2026-10-25, task 6.3a) Worker-supervisor tests drive the real class, not
  a duplicated fake controller: `WorkerController` moved from the
  `openshow-ui` executable into `openshow-ui-core`, and
  `WorkerController::start()` honours the test-only
  `OPENSHOW_WORKER_PROGRAM` override so fixtures spawn the test binary
  itself as a scripted JSON-RPC peer. Replies correlate by request id plus
  a connection generation (decoder reset with it); the latest-go rule lets
  only the newest go's prepare acknowledgement move the audience.
- (2026-10-25, task 6.3b) The restore promise is CONTENT, not a store
  handle: the first clear of a sequence retains the pre-clear scene JSON
  and `restoreOutput()` re-prepares it, so worker store pruning cannot
  retire the operator's undo; the retained intent lives until a status
  poll confirms the restored revision on air and survives error replies so
  retries stay available. Clears derive from the newest go's payload (the
  pending logical presentation) so successive clears compose without
  waiting for confirmations.
- (2026-10-25, task 6.4) Status's applied fields describe the newest
  SUCCESSFULLY PRESENTED frame, never the moment a command was noticed:
  the run loop captures one immutable `FrameSnapshot` (with a per-scene-id
  content pin) under `fMutex` at the present decision, the frame draws
  exactly it, and only a successful present commits applied
  mask/monitor/scene/revision; failures publish nothing, retry, and bump
  `presentFailures`. Queued/transitioning/confirmed stay distinct and
  pending recomputes at every commit. Preview tags name drawn content.
  Fault injection is env-gated, render-thread-only, and re-armed per
  scene change so fixtures force the command-during-apply and
  present-failure windows instead of racing them.
- (2026-10-26, task 6.5) A preview release must PROVE ownership: the
  exact published sequence (`seq == published && released < seq`, the old
  watermark was the bug) plus the live stream's segment name, which
  `preview.ready` carries and `preview.released` echoes. The rule is
  stricter, not additive, so capability `preview.stream-id.v1` (minor 4)
  gates it; consumers gate readys by name and never copy or release under
  a foreign stream. Queued notifications of a retired stream are dropped
  at dequeue, and `create()` resets the per-slot bookkeeping because a
  new segment is a new ownership life.
- (2026-10-26, task 6.6a) A GPU presentation token is a LEASE, not a
  per-flush borrow: while the session displays a frame its slot stays off
  the producer's table, and each sampling flush only adds a done-semaphore
  obligation the slot's next commit waits on. Every handback PROVES its
  generation — the exact decoder sequence leasing the token, which is the
  proof because sequences grow monotonically, are never reset, and a
  producer cannot re-stamp a slot whose lease has not ended. `FrameRing::
  claim()` is round-robin by contract: a from-index-0 free scan starves
  every other slot the moment a consumer holds one frame while the
  producer cycles, and leases make that degeneration observable. A
  CPU-synchronous preview readback needs no doneVk because the lease keeps
  the slot untouched through its draw.
- (2026-10-26, task 6.6b) The transport's generation boundary is
  stop/join, not a null-store: the session may write the seam's presenter,
  clock and pending only after the pipeline's producer is joined, because
  the producer reads the presenter pointer unsynchronised — no null store
  synchronises a race, only the join does. A failed start() needs no such
  boundary because every failure returns before any thread spawns. A
  "no callback after join" regression must sample the instant join
  returns; sampling after close() lets close's own teardown mask a
  detached or early-returning join.
- (2026-10-26, task 6.7a) Stopping a threaded transport and restarting it
  is a STATE lifecycle, not a flag: join() reaps the thread, reset()
  re-arms the state — the mixer's started gate, input table, queues and
  counters, the sink's wait/readiness/activation/clock/stats fields. A
  failed start() must self-clean the same way, because the inputs it
  registered before failing shift the retry's expected indices. The
  discriminator for stale transport state is the read the INSTANT the new
  run begins: a fresh sink connected INACTIVE has zero callbacks and is
  not active, so any nonzero/active reading is the previous run's residue
  (and a "restarted" fixture whose waits read counters the previous stop
  zeroed can only pass on the new generation's work). PipeWire's Format
  param arrives AFTER the PAUSED readiness event, so a sink's negotiated
  rate is legitimately 0 the moment start() returns — every generation;
  fall back to the preferred rate and let server stream-time re-latch the
  clock, and check negotiation after the first callbacks.
- (2026-10-26, task 6.7b) A source identity must be a NAME, not a table
  position: index-equality checks (`addInput() == kSecondInput`) fail
  whenever the other source is absent, and the failure mode is silence
  returned as success. Map identity -> assigned index at registration,
  re-arm the map with the table it indexes, and look up per-source stats
  through the same map. A fallback media clock needs the same contract
  as the audio clock it replaces — hold on pause, continue on play, land
  on seek, and no position move the audio did not make — and a clock
  function a caller may capture BEFORE start() must answer from
  construction: an un-anchored steady_clock reading silently becomes
  machine uptime and paces the whole scheduler by it (bench drift in
  tens of thousands of seconds; the fix's proof is a bounded reading
  plus one honest bench rerun).
- (2026-10-26, task 6.7c) A status field derived from a component's own
  success flag is not a report — derive it from what the audience can
  receive. "Sink up" was true while every source had failed to open, so
  silence was reported as success; the fix is a separate expectation
  (audio was wanted) from an outcome (N sources opened, sink ready,
  activation accepted, transport held), typed into named lanes so each
  silence says who owes it. Two corollaries: an ASSET fault outranks
  machine state (a broken file must never read as "output switched off"),
  and recovery must be typed to the class AND asset that recovered, because
  a per-frame reconcile of the healthy on-air transport will otherwise
  erase a queued asset's standing fault. Related: a status refreshed only
  by frames cannot report the death of the thing that produces them — the
  event-driven loop presents nothing once frames stop, so a heartbeat must
  reconcile it; "playing" is a frame-time claim (credit on submitted
  frames, never at open); and a cached failure may be reused by the hot
  path but never by an operator action, since the repair is a new file at
  the same path. Proof discipline: FFmpeg serving a truncated mp4 from its
  AVIO/packet cache makes partial truncation legitimately loopable, so a
  producer-level fixture cannot isolate a no-progress guard — pin such
  guards where the observable behaviour lives, and say so.
- (2026-10-26, task 6.7d) A deferred request is a PAIR, never two reads:
  whoever performs it takes `(generation, target)` together and credits only
  that generation, because reading the newest target and then acknowledging
  the newest count settles a request that arrived mid-apply unperformed — and
  the reader of "nothing pending" is a drain loop, which then presents the
  superseded position. A failed request needs TWO terminal states, not one:
  APPLIED and REFUSED. Refusal is the honest answer for a jump one part of
  the transport could not make — flush nothing (no queued sample is stale),
  do not move the applied watermark or the heard position, count it, and
  report the target that was refused; treating it like success destroys the
  only evidence the audience has that playback did not jump. Related, in the
  same transport: media position and present time are DIFFERENT domains and
  must be carried as two fields with one comparison site, because they
  disagree by exactly the amount a pause, a seek or a loop wrap introduced —
  and a wrap makes pts larger than the clock by a whole clip. A producer
  parked waiting for a resource that only the consumer's next step frees,
  where that step is what the producer's own pending request prevents, is a
  deadlock no timeout hides: fix the release paths (every frame that will
  never be handed onward returns its slot NOW, including the ones a flush
  purges), and let a stale frame abandon its wait — raising "abandoned" when
  a request is issued and clearing it only where the audience is actually
  served, since a request can be issued, performed and settled between two
  passes of the clearing thread. Pinning: a window microseconds wide is not
  testable by racing it — add a production seam that parks the thread inside
  the window (precedent: the `OPENSHOW_FAULT_*` knobs) and shape the scenario
  so a swallowed request cannot drift into its target by luck (older request
  forward, newest backward). Scrub semantic chosen deliberately: a seek
  issued while held PRESENTS its target and parks again (a frozen clock makes
  "now" the only honest deadline, showing the target is the point of a parked
  scrub, and the offer is the seam's way out of a slot-holding hold); status
  must reflect an operator op on the pass that applied it, never on the next
  heartbeat.
- (2026-10-26, task 6.8a) A control message is a request to change what the
  audience sees, never a container for content: the byte budget belongs to the
  transport and is honoured by BOTH ends (a sender that builds a frame the
  peer's decoder refuses is the author of someone else's disconnect), nesting
  gets its own ceiling because depth is what a payload still has once it fits
  the bytes, and every echoed string is clipped on a UTF-8 boundary because a
  refusal that quotes the problem must not repeat it. How big authored CONTENT
  may be is an application rule, so the budgets live in `openshow::scene` next
  to the type they bound and the client runs the worker's own reader before
  writing: the two sides cannot disagree, and "rejected locally" means the
  operator's paste never costs them the worker. Refuse-and-keep by default,
  refuse-and-drop only where the stream itself is no longer interpretable —
  an argument with a problem is a typing mistake, a frame without framing is
  not. An identifier that cannot be answered honestly is answered under 0
  rather than narrowed: replies correlate by id, so a wrapped value applies
  the WRONG command's acknowledgement, which is worse than no answer. A command
  with no id is not executed: an update nobody can acknowledge is exactly the
  lost-update shape task 6.3a closed, and "silently did it anyway" is the one
  outcome the operator cannot diagnose. Range checks happen where the number
  arrives, never after the cast — `static_cast<int>(4294967296)` is 0, a real
  monitor index. A scene's key set is CLOSED: this payload is the audience's
  content, so a field this build does not read is a field that silently never
  appears on air, and compatibility belongs to the format number, not to
  ignoring keys. Media references name local files only, because a `data:` URI
  is a media-byte payload in a control message and a URL is a fetch the worker
  has no contract for. The client clears no state for a send it did not
  perform: the restore snapshot a clear was holding survives a refused
  present. Finally, a budget is only a number until it is MEASURED against
  the supported model — the largest scene the budgets allow and the deepest
  message the protocol sends are printed by the fixtures and pinned against
  the transport's limits, so the two can never drift apart silently.
- Layout and names as in the table: `openshow-ui` and `openshow-worker` are the
  two shipped executables, with `src/domain` and `src/protocol` Qt-free and
  shared by both.
- The worker must link no Qt. `tests/CheckNoQt.cmake` inspects `ldd` output, and
  the inverted check on the frontend proves the guard is not vacuous.
- The worker gains no SDL dependency: GLFW provides the output window and Vulkan
  surface, vk-bootstrap the instance and device, following `/home/jim/sloppy-ui`.
- Audio is native PipeWire: one stream per worker, software mix of embedded
  video audio with one standalone track, per-source volume and mute, and the
  process callback as clock.
- Sockets and JSON settled: a `poll()` loop on a raw Unix socket and Boost.JSON,
  with no Asio and no nlohmann/json.
- Text layout stays in Skia with SkParagraph. There is no shader toolchain:
  Skia compiles SkSL to SPIR-V in process, so a custom effect is SkSL through
  `SkRuntimeEffect` rather than hand-written SPIR-V.
- No-compositor output through the Vulkan display plane is deferred.
- Keep progress in these tracked files rather than chat memory, and never mark
  authored scaffolding as verified graphics work.
- 2026-10-23, raised as "when does the server architecture stop being
  TestScene?": the scaffolding graduates **per task, not as a separate
  cleanup milestone**, because the render API's shape must be forced by the
  protocol and scene types that do not exist yet. PLAN.md gained a *Worker
  architecture graduation* section: 1.2 must carve the session seam (an
  object owning window/swapchain/LayerStack/output state that RPC handlers
  call; `--render-test` becomes its development client), 1.3 moves display
  selection behind session operations, 1.4 requires the scene to be data
  (preview targets), 2.4 delivers the compiled-scene type, 3.1/3.2 replace
  the five hard-coded layers, and `TestScene` ends as a test fixture.
  Review rule: a task does not pass by threading a command to hard-coded
  scene state; the seam comes first. What is *not* debt: `src/worker/output/`
  and `render/Layer.{h,cpp}` are already production-shaped.
- (2026-10-23) Preview transport decisions (task 1.4, inside the user's
  constraint that all rendering happens in the worker and the JSON API
  carries references, never bitmaps): a preview frame is the audience
  composition read back offscreen — same Vulkan/Skia backend
  (`SkSurfaces::RenderTarget` + `readPixels` on the output's
  `GrDirectContext`), downscaled 640x360@15fps is the sanctioned
  approximation, no CPU raster twin; one shm segment with three seqlock
  slots carries the pixels while notifications ride the RPC socket; the
  producer may drop frames but the output never blocks on a consumer;
  the `preview.start` reply names the segment yet the consumer maps
  only on the first `preview.ready`; `PreviewStream` is held by
  `std::shared_ptr` across session and RPC so teardown can never race a
  release.
- (2026-10-25, task 6.2a) GPU memory allocation is owned by one adapter
  (`output/VulkanMemoryAdapter`) over the system VulkanMemoryAllocator,
  written to Skia's public `VulkanMemoryAllocator` contract — the upstream
  answer to this Skia revision's mandatory `fMemoryAllocator`. The system
  VMA is shared deliberately (user decision): it is the one private-tree
  concept worth keeping, and it stays confined to a single TU with
  `VMA_CALL_PRE static` so libskia's exported VMA copy is a non-issue.
  Vulkan entry points are scope-resolved (instance GIPA for physical-device
  functions, GDPA for device functions) because this platform's loader can
  return raw ICD pointers from instance GIPA that crash on loader handles.
- (2026-10-26, task 6.10) A cache for show content has two different
  bounds and they must not be confused: the BUDGET bounds the transient
  (backgrounds a service pushed past and will never show again), and the
  PIN SET is content the operator can put on air at any moment, which is
  kept and whose excess over the budget is REPORTED rather than paid for
  with a colour fill mid-show. The primitive that keeps that honest is
  re-derivation, not reference counting: syncPinSet releases every pin and
  then takes one per DISTINCT named path, so any number of repeated
  prepares lands on the same truth, and a path the caller stops naming is
  exactly a reference that ended (no shadow bookkeeping to drift). Three
  ordering laws came out of it: gates run BEFORE pixels are allocated (the
  file's stat, then the codec's declared dimensions — a refusal that has to
  allocate first is not a gate); the lookup the draw path uses never touches
  the disk (a miss answers the colour fill, and both directions are
  testable: an arriving file changes nothing for a lookup, a deleted file
  changes nothing for a hit); and the budget resettles only after the
  re-pin — evicting between the release and the re-pin strips the oldest
  prepared scenes to placeholders, because "oldest" and "prepared" are the
  same entries in a real show. A failure is a zero-byte placeholder: the
  draw path may not retry at frame rate, but an operator action always
  retries, and eviction forgetting the placeholder is another path to the
  same recovery.
- (2026-10-26, task 6.10) A degradation the audience sees must reach the
  wire. A still that failed to decode was reported at prepare (6.7c), but a
  picture that was there at prepare and gone at apply produced a colour fill
  with a clean status — and that asymmetry was only discovered because a
  mutation (leave the store out of the pin set) survived a "no media.error
  was pushed" check. Rule: when a fixture asserts the ABSENCE of a report,
  prove the code is capable of producing it, otherwise the check is a
  tautology. Two more fixture laws this task paid for: the arithmetic of a
  fixture is part of its claim (512x512 N32 is 1 MiB, not 2 — the first
  flood of 100 sat UNDER the 192 MiB budget and every eviction check passed
  vacuously), and the geometry must sit where the rule bites (a
  pins-under-budget scenario cannot see evict-mid-release; the scenario that
  catches it holds 90 pins above the budget and re-derives the same set).
  One owner per ledger too: mixing hand pins with set syncs in one fixture
  tested neither, and its counters were unexplainable until the fixture
  used syncPinSet alone, exactly as production does.
- (2026-10-26, task 6.8b) An RPC server's outbound queue is shaped by two
  rules and the transport's own number: its byte budget IS the frame budget
  (a queue that buffers what the wire would never carry is a lie), and its
  entry cap (1024) exists because the coalescing scan is linear in it. A
  REPLY is sacred — losing one leaves a client unable to determine a
  command's outcome, worse than a closed connection — so room is made only
  from notifications, and never from one already part-way down the wire,
  whose sent bytes are in the peer's stream and must stay aligned; a refused
  reply is honest grounds to name the budget and close. The write deadline
  measures PROGRESS (last byte the peer took), not elapsed debt, because
  "slow" and "gone" are different peers and only one of them deserves
  hanging up. The queue is drained on every poll tick, not on POLLOUT:
  POLLOUT promises room, and a small-buffered trickle reader keeps it off
  for seconds while reading all the same — gating on it recreated the very
  stall the queue exists to remove (T5 caught it). fConnectionFd gets a
  single owner by construction — the serving thread closes, stop() may only
  shutdown() under fConnectionMutex — and TSan is the proof, not the
  fixtures: races are invisible to tests that pass on the right
  interleaving. On this host TSan needs the libtsan rpm unpacked locally
  and gcc's linker script shadowed with -B; its reports must be read as a
  DIFF against the ~2450 pre-existing glib/pipewire allocator races, not as
  a zero. Fixture law this task paid for three times over: never measure a
  bound against the constant under test (raise it in a mutant and the
  fixture raises with it), pin a rule in the place it is observable
  (notification replacement cannot be seen through a live worker whose
  render thread gates each slot), and a trickle loop needs its sleep and
  its maxBytes-capped recv or it is silently a different scenario.
- (2026-10-26, task 6.8b) The UI's side of the same finding: a request
  bound (kMaxInFlightRequests = 64) refuses to build the queue the worker
  would never answer, and the write-stall watch is ARMED BY A REFUSAL rather
  than running always — its verdict is idle time since QLocalSocket's
  bytesWritten reported a byte, because bytesToWrite() drifts ~100 KiB
  against a peer that is reading, while bytesWritten is the kernel saying
  the peer took one. A connection is closed only when bytes are still
  queued after the window; a peer that resumes inside it keeps its session
  and its backlog.

## Output window log

The projector is the second display and it is unplugged, so output behaviour is
logged here from what is observable off screen rather than claimed as seen.

- 2026-10-21, FreeShow 1.6.7, one display attached, output window created but
  never shown (`autoOutput: false`, top-bar output button carrying class
  `display off`):
  - The output window exists as its own page target while hidden **and keeps
    rendering**: after a slide went to air its DOM held the item, after a clear it
    emptied. Reading it as pixels needs `fromSurface: false`. It occupies the
    configured 1920x1080 bounds (1430x805 CSS at scale 1.34375).
  - Off air: `div.slide` with `background-color: black`,
    `transition: 800ms background-color`, `aspect-ratio: 16/9`, no children.
  - On air with the Welcome slide: white bold "Welcome!" centred, captured at
    `third_party/cache/freeshow-visuals/06-output-slide.png`.
  - Cleared with `.`: DOM emptied, still black, "Clear all" became
    "Restore output". Inline `zoom`, the layer list and the 5% drag strip are in
    `docs/reference.md` §7.
  - Timers: a 100ms interval in that occluded, unfocused window fired 50 times in
    5008ms, so nothing is throttled while the audience window is off screen.
  - Still untestable without the projector: placement and mode switching on a real
    panel, per-display DPI, the display-selection precedence of §7, always-on-top
    stacking against a real desktop, and how transitions and the `blur` fit read on
    hardware. These are open for 1.3 and M4.
- To repeat once the projector is connected: capture FreeShow again with
  `tools/cdp-inspect.mjs` (`shot` with `CDP_FROM_SURFACE=0` for an off-screen
  window, plain `shot` plus `CDP_FRONT=1` for one that is on screen), then do the
  same against `openshow-worker` and compare. Record display topology, reported
  scale factor and window bounds alongside every capture.

## Historical blockers and remaining work (see current checkpoint)

- **Compositor caveat for fullscreen output (found demoing M2)**: this
  machine's second display (DP-4) runs rotated at 1.5× fractional
  scale, and Plasma 5's KWin mishandles xdg-fullscreen windows there —
  it re-sent window configs repeatedly (each one forcing a swapchain
  recreate, which the resize path absorbed correctly) and dropped the
  fullscreen window to minimized when focus moved to the UI. Not an
  app bug: `--render-test`/socket fullscreen is fine on an unrotated,
  unscaled panel. For demos on this machine, windowed output via
  `OPENSHOW_WORKER_EXTRA_ARGS="--monitor -1"` is well-behaved, and the
  Destination dropdown (or the worker's own monitor pick) still goes
  fullscreen elsewhere. Re-test fullscreen on the projector in M3
  acceptance; revisit `--window-monitor` semantics if Plasma 6 changes
  the behaviour.
- No blockers. The earlier screen-lock and unplugged-DP-4 incident
  resolved itself when the user unlocked and reconnected; task 1.1 was
  accepted on the second display (see the task 1.1 log).
- Second-display hardware checks that remain open belong to their tasks:
  projector DPI and always-on-top stacking in 1.3, transitions on
  hardware later.
- Two reference questions need controlled media rather than a running FreeShow and
  are now M4 acceptance items: the pixel result of the `blur` fit with edge
  blending, and per-slide action side effects on go.
- The project's M0 note that vk-bootstrap was unused is superseded: task 1.1
  uses it.

## Historical M1–M5 handoff (next action superseded by 6.1a above)

1. **M1 is complete and tagged**: tasks 1.1–1.5 (1.5a–1.5j, including
   the user-inserted 1.5g validation-layer option, the 1.5h/1.5i
   GPU-slot video path and the 1.5j direct `get_buffer2` decode) are
   DONE, committed and pushed to `dattrax/OpenShowQt` main; the
   annotated tag **`M1`** marks `68062a6`. Gates as of 1.5j: desktop
   ctest **12/12**, headless **9/9** (now incl. `gpu_decode_tests`),
   both GPU test paths pass under `OPENSHOW_VULKAN_VALIDATION=1`, and
   the direct path benches 0 missed with the cached decoder-allocation
   fix (see the direct-decode memory regression log).
2. **M2 is complete**: 2.1 (document model), 2.2 (service packages with
   asset copying and validating load), 2.3 (snapshot undo/redo with the
   id watermark, fingerprint-driven autosave) and 2.4a–c (compiled
   scene type + compiler, worker scene store with acknowledged
   revisions, Qt adapters + demo fixture) are DONE, committed and
   pushed; tag **`M2`** on main's tip. Protocol minor 2 with the
   `scene.store.v1` capability.
   **M3 is under way**: 3.1 (production scene typography through
   `SceneTextEngine`, scene-aware background/words layers, the
   shared_ptr hand-off from the session, and the pixel fixtures), 3.2
   (cover-fitted image backgrounds with a path-keyed decode cache, the
   two-track fade — media 800ms/text 500ms — and cut, `scene.present`'s
   optional `"transition"` parameter, `--check-transition` and four new
   pixel fixtures), 3.3 (the `DocumentController` document session +
   lyric panel: create/edit songs and slides, debounced text with full
   undo, service save/load with the autosave startup rule, and the
   end-to-end proof that a newly created song presents on a live worker)
   and 3.4 (independent clear controls as *derived presentations* with
   acknowledged live state: `SceneEdit` transforms the on-air scene JSON
   — words or background — and presents the result, `WorkerController`
   mirrors each prepared revision's JSON and lights `wordsOnAir/
   backgroundOnAir/canRestore` only from the status-confirmed active
   revision, and restore re-presents the pre-clear revision) are DONE,
   committed and pushed. The **M3 checkpoint demo** then ran the full
   operator story — create a two-slide service in `DocumentController`,
   save it, reload it in a fresh session, present each verse over a
   background image, and exercise every clear independently plus a
   single-step restore from black — capturing the worker's real output
   over the preview stream. It caught a genuine bug: the 1.5e
   event-driven loop slept the moment a crossfade began (one frame at
   blend 0, then quiet — the audience never saw the new scene while
   `output.status` reported it active). Fixed by re-arming
   `fNeedsPresent` after each drawn frame while a fade runs, pinned by
   the Qt-free `live_fade_tests`. Gates: desktop ctest **19/19**,
   headless **12/12**. **M3 is complete.** Next milestone:
   **M4 — the workspace and asset management** (import images/video,
   the media library, rename/reorder/groups). **4.1 is DONE** (the shell:
   40px top bar, the three regions, `WorkspacePanels` with the reference's
   exact resize snaps and persisted widths, dark Material theme, the
   `OPENSHOW_UI_SIZE` capture seam; layout pixel-probed at 1680x1020).
   **4.2 is DONE** (the searchable library drawer — eight tabs with Shows
   and Media live, the 300/40 height with the reference's snap and
   per-session reopen, the Ctrl+F search with the normalised word-start/
   tier contract and Enter-to-insert placement — plus playlist chips,
   drag/▲▼ reorder handles and the media registry).
   **4.3 is DONE** (authored per-slide text boxes end to end — domain
   fields with the pixel-identical default, storage that reads legacy
   manifests, a per-box renderer with per-item shrinkToFit, the edit page
   with drag/resize/nudge/X-Y-W-H and one-undo-per-gesture, and the page
   key scheme).
   **4.4 is DONE** (style typography end to end — `Style.align`/
   `lineSpacing` with the draw-identical defaults, one shared validator,
   storage round-trip with the legacy-style default and load-time
   honourability checks, required wire fields, renderer alignment and
   per-item line box, the edit-page STYLE panel (combo, New/Show
   default/Follow show, name/colours/font/size/align/line-spacing with
   commit-on-focus-out and revert-on-refusal) and the background-image
   picker, a style-bound canvas representation, and the controller
   fixture proving one style edit lands identically on every following
   slide's compiled scene).
   **4.5 is DONE** (the keyboard-driven show — presenter keys with the
   reference's gating and a mutually-exclusive peace with the edit-page
   nudge, undo/redo/save and the slide clipboard as controller ops with
   one undo entry each and stale-reference sanitising, the Ctrl+D
   duplicate-else-drawer rule, opt-in number keys, F11, and a settings
   page that persists through the new `AppSettings` and documents every
   shortcut). Gates: desktop **21/21**, headless **12/12**. The **M4
   checkpoint** then ran the whole story — author (words, box, style
   typography, background, interlude) → save/reload → clipboard →
   presenter walk → clears → restore — against a live worker with
   pixel proofs, all green; its first-run failures were all demo-side
   expectation errors, the notable one being that save *archives media
   into `<service>/assets/`* so a reloaded scene legitimately resolves
   a different `assetPath` (the PLAN's storage rule, now asserted
   honestly). **M4 is complete** — committed and pushed, annotated tag
   **`M4`** on main's tip. Next milestone: **M5 — video and audio**
   (video decode/presentation, embedded and standalone audio mixed,
   volume/mute per source).
3. **M5 is under way**: **5.1, 5.2, 5.3, 5.4 and 5.5 are DONE**. 5.1 graduated
   the bench glue into the production `PlaybackPipeline` (Qt-free
   `playback/` library: decode thread, GPU ring, paced submission,
   presenter role), with `playback_queue_tests` pinning boundedness.
   5.2 made playback controllable through the real seams (decode-thread
   seek flush + skip-before-claim, pacing-side drain and clock
   re-anchoring, pause parking on the bounded ring, live loop toggle;
   `playback_control_tests`, five scenarios). 5.3 put the audio behind
   production `openshow::media::AudioMixer`: bounded SPSC ring per
   input, per-input volume/mute atomics with a 50 ms per-frame gain
   ramp (muted inputs keep draining), per-input starvation counting,
   the sum delivered through an injected `WriteFn` into the one
   PipeWireSink; `audio_mix_tests` pins the exact mix math, the
   clickless ramp (max jump 0.0004), starvation safety and live
   independent mute at the real decode seam (RMS 0.768→0.300→0.768),
   and `--av-bench --second-audio` proves the live two-input PipeWire
   path (underruns 0, 0 missed). 5.4 graduated the transport itself
   into `openshow::worker::AvPlayer` (sink + mixer + feeders +
   pipelines): INACTIVE connect with pre-roll-then-`resume()` as the
   clock anchor (queue depth is latency, never skew), pause/resume
   that never flush, seek as the only re-anchor (feeder `seeked`
   callback → per-input queue drop → sink flush → clock rebase), the
   loop-locked `pw_stream_set_active` marshal (foreign-thread
   activation silently fails with EPERM — real bug, fixed), and a
   monotonic fallback clock without a daemon; `av_sync_tests` (60
   checks, both presets) plus live reruns met the M5 targets: 60 s run
   missed 1/1501 (0.07% < 1%), drift mean +0.6 ms max 37.0 ms (< 50
   ms), sink underruns 0, mixer starved 0 (was 201). 5.5 put it all on
   air: `Background::isVideo` (kind from the extension, wire-optional,
   part of the continuity identity); a real `PlaybackPipeline` +
   `AvPlayer` transport in `OutputSession` that **never restarts for a
   lyric change** (identity unchanged → untouched — proven by the loop-
   continuous `media.position` climbing *through* the change), cuts the
   media track across transitions, and lives outside `frame()`'s
   graphics-queue lock (its teardown joins threads that may block on
   that lock — live deadlock found and fixed); `scene.prepare` preloads
   (stills decode on the RPC thread into a now-thread-safe
   `SceneImageCache`, videos probe), so a 4K JPEG costs 43 ms at prepare
   and 0 ms on re-prepare; failures became visible — `status.media
   {playing, path, error}` with a sticky error plus the `media.error`
   notification (protocol minor 3, capability `media.error.v1`, never
   re-notifying an identical repeat), amber top-bar surface in the UI.
   `background_media_tests` pins the acceptance live over a real
   `--socket` session (self-generated silent fixtures; missing/corrupt
   files push matching notifications, recovery clears; hiding the
   background layer keeps playback rolling) and measures **p95 18.7 ms**
   present→air against the 100 ms target. It also caught a real
   scheduler bug from the scene-apply move: `frame()` captured
   `presentedFor` *after* the apply, so a command landing during the
   apply was credited to a frame that never drew it — back-to-back
   presents stalled on air; the revision is now captured in the run
   loop's decision critical section (the 1.5e invariant, placement now
   load-bearing). Gates: desktop **26/26**, headless **16/16**.
   **M5 is complete** — committed and pushed, annotated tag **`M5`** on
   main's tip. Next milestone: **M6 — reliability and installable
   alpha**.
4. Useful carry-over: the preview throttle proves the session can run a
   secondary cadence (15 fps) without disturbing the output loop — a
   video layer's frame-pick cadence hangs off the same seam;
   `PreviewStream`'s claim-only-released / drop-not-block semantics are
   the template for any bounded producer–consumer queue here;
   `WorkerController` now routes worker→UI notifications, so more
   event types keep working. `--av-bench` is the ready-made harness the
   moment video layers need real decode-to-present plumbing, and
   `--vulkan-validation` keeps any new hand-written Vulkan honest.
5. One manual check stays open from 1.3: drag-resize the borderless output
   window once and confirm the swapchain recreate (see the acceptance
   caveat in the task 1.3 log).

## Resume checklist

1. Read `PLAN.md` and this file.
2. Inspect Git status/history and any existing changes.
3. Continue the current task; preserve unrelated work and the source clone.
4. Before stopping, update this file with commands, results and the next action.
