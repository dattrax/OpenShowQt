# OpenShowQt progress and session handoff

Updated: 2026-10-23 (Europe/London).

## Current checkpoint

- Milestone: **M1 — Prove frontend-to-renderer integration**, tasks **1.1**
  (2026-10-22) and **1.2** (2026-10-23) DONE. M0 was closed earlier with tag
  `M0` at `bca7cee`; commits and tags are delegated to the agent for this
  session.
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
- Checks: `ctest --preset desktop` passes **8 of 8** (adds
  `worker_rpc_lifecycle`, labelled `display`); `ctest --preset headless`
  passes **6 of 6** — the headless test preset now excludes the `display`
  label, which needed the CMake-4 `filter` key rather than `tests`;
  `worker_is_qt_free` stayed green throughout, so the RPC path is
  `poll()`/Boost.JSON with no Qt.
- Next: task **1.3** (display selection and swapchain recreation over RPC,
  including `--monitor` moving behind session operations).

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

## Verification and environment

- 2026-10-23, task 1.2: `ctest --preset desktop` passes **8 of 8** — the
  seven task 1.1 tests plus `worker_rpc_lifecycle`
  (`tests/rpc_ipc_tests.cpp`, LABELS `display`, TIMEOUT 90).
  `ctest --preset headless` passes **6 of 6**; `ctest --preset headless
  --show-only` confirms the `display` label exclusion so the new test,
  which opens a window, is not counted against the no-Qt preset.
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

1. Tag and commit the M1 half that is now provable — the milestone itself
   is not closable yet: tasks 1.3, 1.4 and 1.5 remain TODO. Start **1.3 —
   display selection and swapchain recreation over RPC**. Per PLAN.md's
   graduation note the first move is to retire the `--monitor` flag's
   placement logic behind session operations: add `output.displays`
   (enumerate the GLFW monitor list) and `output.select` (destination by
   index or `-1` for windowed) to `RpcServer`, each calling a new
   `OutputSession` method that re-enters `glfwSetWindowMonitor` and lets
   the existing `extentOutOfDate()` path rebuild the swapchain. Acceptance
   is "fullscreen, resizing and switching work" **seen on a panel**; the
   projector is still not connected, so record off-screen evidence labelled
   as such and keep the physical check open.
2. Useful carry-over from 1.2: the UI already polls `output.status` at
   2 Hz and renders layer checkboxes, so a destination picker drops into
   the same `WorkerController`/`Main.qml` pair; the frame-30 readback probe
   (`Options::diagnostics`) and the `display` test label are both in place.

## Resume checklist

1. Read `PLAN.md` and this file.
2. Inspect Git status/history and any existing changes.
3. Continue the current task; preserve unrelated work and the source clone.
4. Before stopping, update this file with commands, results and the next action.
