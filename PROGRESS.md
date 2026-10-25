# OpenShowQt progress and session handoff

Updated: 2026-10-25 (Europe/London).

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
- Working changes: PLAN.md milestone replacement, this handoff, and new review
  document only. The pre-existing OutputSession.cpp clock log is unchanged.
  User authorized committing and pushing the three documentation files on
  2026-10-25 before implementation begins. Documentation checkpoint message:
  `docs: review M5 code and plan M6 remediation before M7 productization`.
- Exact next action: **6.1a**, production compositor startup/clear black and
  clock/diagnostics-off defaults, with pixel checks at the real output/preview
  boundary. Then execute the focused M6 leaves in order. Review completion
  does not mark any finding fixed; no new implementation blocker was found.

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
