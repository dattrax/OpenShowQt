# OpenShowQt progress and session handoff

Updated: 2026-10-23 (Europe/London).

## Current checkpoint

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

## Blockers and remaining work

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

## Exact next action

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
   pushed; tag **`M2`** on main's tip. Gates: desktop ctest **13/13**
   (new `scene_tests`), headless **10/10**; protocol minor 2 with the
   `scene.store.v1` capability. Next milestone **M3 — working lyric
   presentation**: production output layers that *draw* the active
   scene (the session already applies and reports it), which retires
   the hard-coded TestScene content behind scene-driven rendering.
3. Useful carry-over: the preview throttle proves the session can run a
   secondary cadence (15 fps) without disturbing the output loop — a
   video layer's frame-pick cadence hangs off the same seam;
   `PreviewStream`'s claim-only-released / drop-not-block semantics are
   the template for any bounded producer–consumer queue here;
   `WorkerController` now routes worker→UI notifications, so more
   event types keep working. `--av-bench` is the ready-made harness the
   moment video layers need real decode-to-present plumbing, and
   `--vulkan-validation` keeps any new hand-written Vulkan honest.
4. One manual check stays open from 1.3: drag-resize the borderless output
   window once and confirm the swapchain recreate (see the acceptance
   caveat in the task 1.3 log).

## Resume checklist

1. Read `PLAN.md` and this file.
2. Inspect Git status/history and any existing changes.
3. Continue the current task; preserve unrelated work and the source clone.
4. Before stopping, update this file with commands, results and the next action.
