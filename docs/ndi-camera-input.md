# NDI camera input (ND)

OpenShowQt can show a live NDI camera feed from the room. The source is an
ordinary **camera asset in the Media Bin**: right-click the Media Bin and choose
**Add NDI source…**, pick a discovered source, and the camera becomes available
as a background/media item. Video only; NDI audio is ignored.

ND is an **input** feature. It is separate from the planned NDI *sender* in FO.3/
FO.4 (lower thirds to OBS), and it is not a slide context or an output
destination. It is optional and not part of the default build.

## Licence boundary

The NDI SDK is proprietary. It must not be linked into the GPL application.

- The GPL application contains only the generic live-source hub, IPC client and
  shared-memory frame reader. It has no NDI headers or symbols.
- A separate executable, `tools/ndi-helper`, links the NDI SDK and owns
  discovery and receiving. It is licensed **MIT** (see
  `tools/ndi-helper/LICENSE`) so it can carry the NDI EULA obligations without
  claiming the application's GPL terms.
- The application launches the helper and speaks a documented JSON protocol over
  pipes; frames arrive through POSIX shared memory. A helper crash cannot take
  down the operator session.
- The helper is distributed separately and is optional. The application builds
  and runs without it; the NDI action is absent when no helper is present.
- NDI attribution/trademark text and the NDI third-party licence are part of the
  helper's distribution, not the application binary.

The repository's `LICENSE` is the GPL text for the application. The helper's
directory carries its own MIT `LICENSE`; `docs/ndi-camera-input.md` records the
boundary so packaging cannot accidentally merge them.

## Operator flow

1. Right-click the Media Bin (or its header) and choose **Add NDI source…**.
   The action appears only when the worker advertises the NDI plugin capability.
2. Pick a source from the discovered list. The source name is stored in the
   camera asset; no address is hard-coded.
3. The camera appears in the Media Bin with a camera badge. Use it as a slide
   background (v1: full-canvas camera feed) like any other media asset.
4. If the helper or camera disappears, the feed shows a placeholder and a
   retry action; the rest of the session keeps running.

The source picker is deliberately a list of names, not a free-text URL. NDI
source names are what the discovery protocol returns and what the receiver
connects to; a typed name would be a second, unvalidated spelling.

## Architecture

```text
GPL application (Qt frontend + worker)          Separate helper (MIT, NDI SDK)
  DocumentController                              openshow-ndi-helper
    -> Documents/History  (camera asset)            -> NDI discovery
    -> scene compiler (camera background)           -> NDI receiver thread
    -> WorkerController --- IPC --->                -> frame ring (shared memory)
         -> LiveSourceHub  --------------------->    -> JSON control pipe
         -> LiveFrameStore  <--------------------   frames
         -> BackgroundTransport / SceneRenderer
```

- The worker owns the generic `LiveSourceHub` and `LiveFrameStore` (ND.6). It
  launches the helper on demand, sends control commands and maps the frame ring.
  It contains no NDI headers or symbols; a CI guard checks the app binary.
- The helper owns NDI discovery and receiving. It is a separate executable with
  its own repository/submodule, licence and NDI attribution. It can be omitted
  entirely; the app still builds and runs.
- The worker's `HelperBackend` and a `FakeBackend` implement the same
  `LiveSourceBackend` interface (ND.3, `src/worker/live/`). `HelperBackend`
  starts the process with `posix_spawn`, speaks the protocol over pipes and maps
  the segment with `ShmFrameReader`; the fake publishes the same ring from a
  thread. The fake backend is always built for tests; the helper backend is
  generic IPC and part of the GPL app.
- The frontend never sees NDI. It asks the worker for capability/sources and
  stores the chosen source as a camera asset in the document.

### Helper IPC and frames

- Control: line-delimited JSON over the helper's stdin/stdout. stdout carries
  protocol messages only; diagnostics go to stderr. Commands: `probe`, `list`,
  `open`, `close`, `quit`. The helper answers with JSON objects and may emit
  `event` objects (source lost, receiver error).
- Frames: a bounded shared-memory ring per open source. The shared header lives
  in `shared/openshow/live/frame_ring.h` (MIT, fixed-size POD) so the helper and
  the GPL app agree on the layout. Each slot carries a header (width, height,
  stride, format BGRA/RGBA/UYVY, alpha, sequence, timestamp, source name) and
  pixel bytes. The helper writes the newest frame and overwrites old slots; the
  worker copies the newest into its own pooled frame and uploads. Newest wins;
  no growing queue. No per-frame socket messages.
- Probe: `--probe` returns helper version, protocol version, NDI runtime version
  and capability. The worker runs it with a short timeout at startup/rescan.
  Missing or incompatible helper = no capability and no "Add NDI" menu.
- Lifecycle: the worker launches the helper on demand (first discovery/stream)
  and stops it on idle/shutdown. A helper crash is a typed fault; the app keeps
  running and offers "Retry".

### Media Bin and document model

- Add `AssetKind::Camera` to the media registry. A camera asset has a display
  name and the exact NDI source name; no file bytes. The storage format gains a
  camera kind and a source-name field. The Media Bin shows a camera badge and
  "NDI: source name".
- v1 treats the camera as a **background** media item: the slide's background
  asset points at the camera asset and the scene compiles a camera background.
  The existing background fit (`cover`/`contain`) applies; `blur` is refused for
  live video. A camera cannot be an image foreground item in v1.
- **One multimedia source per cue.** The compositor's multimedia layer holds
  either the live NDI feed, an FFmpeg playback, or a static PNG/JPEG — never
  more than one at a time. A camera background therefore uses the same layer and
  the same geometry as a video background; it does not add a second video layer
  and there is no live-plus-file-plus-still composition to arbitrate.
- The NDI receiver asks for **UYVY** (2 bytes/pixel). The H.264/HEVC stream is
  normally 4:2:0, but the standard NDI receive API exposes YUV only as 4:2:2
  (UYVY, or P216 with `best`); the runtime's 4:2:0→4:2:2 expansion adds no
  source detail. UYVY is still much better than the SDK's BGRA conversion. The
  worker converts UYVY to NV12 because RADV cannot sample a Vulkan 4:2:2
  format, and feeds the existing `GpuYuvUploader` — the same Skia/Vulkan YUV
  path as FFmpeg playback. The ring carries UYVY; the GPU layer receives NV12.
  The ring is half the size a BGRA feed would need.
- **Receive format matrix (real HX source):** `BGRX_BGRA` → BGRX 4:4:4
  (stride 7680), `RGBX_RGBA` → RGBX 4:4:4, and `UYVY_BGRA`, `UYVY_RGBA`,
  `fastest` and `best` all → **UYVY 4:2:2** (stride 3840). Passing
  `NDIlib_FourCC_video_type_NV12` or `_I420` as the color format fails receiver
  creation. NV12 is a frame-layout FourCC, not a receive request; the standard
  SDK has no 4:2:0 output.
- The worker's background transport detects the `ndi://` asset path and asks the
  live hub for frames instead of opening a file. The renderer draws the newest
  frame like a video background. Previews use the same path, so the operator
  sees the camera off-air too.
- The source name is stored in the document; the worker resolves it to a live
  receiver. If the source is missing, the document still opens and the slide
  shows a placeholder.
- Wi-Fi strongly favours NDI|HX: full-bandwidth NDI at 1080p60 would saturate
  the link. HX is the default expectation for phone and PTZ cameras; the HX
  decoder needs the FFmpeg 7.1 libraries described below.

## SDK acquisition and build

The NDI SDK is **not** committed and **not** a git submodule. Download it from
<https://ndi.video/> after accepting the NDI SDK licence, then run:

```sh
scripts/fetch-ndi-sdk.sh /path/to/Install_NDI_SDK_v6_Linux.tar.gz
```

The script extracts the SDK to `third_party/ndi-sdk` (gitignored) and prints the
`OPENSHOW_NDI_SDK_DIR` to use. It also works with `NDI_SDK_ARCHIVE` and refuses
to overwrite an existing SDK without `--force`.

NDI|HX sources (phone cameras, many PTZ cameras) are decoded by the NDI runtime
through FFmpeg **7.1** (`libavcodec.so.61` / `libavutil.so.59`). Fedora and RPM
Fusion ship FFmpeg 9 only and no package provides the 7.1 sonames, so build the
minimal decoder set once:

```sh
scripts/fetch-ndi-ffmpeg.sh
```

It downloads FFmpeg 7.1.1 and builds only the H.264/HEVC decoders into
`third_party/ffmpeg-7.1` (gitignored, about 3.5 MB). **Install `nasm` first**:
the assembly build decoded 1080p60 HX at about **37% of one core**; the C
fallback used about **56%**. The helper stages those libraries beside
`libndi.so.6`, gives itself `$ORIGIN` in its runpath and preloads them with
`RTLD_GLOBAL`, so the runtime's HX decoder resolves them without changing the
system FFmpeg. Full-bandwidth NDI sources need none of this.
`-DOPENSHOW_NDI_FFMPEG_DIR=<dir>` points the helper at an existing 7.1 tree
instead.

Build the helper standalone:

```sh
cmake -S tools/ndi-helper -B build-ndi-helper \
  -DOPENSHOW_NDI_SDK_DIR="$PWD/third_party/ndi-sdk"
cmake --build build-ndi-helper
build-ndi-helper/ndi-helper --probe
```

The helper loads the NDI runtime (`libndi.so.6` / `ProcessingNDI.dll`) with
`dlopen`/`LoadLibrary` from `OPENSHOW_NDI_RUNTIME`, `NDILIB_REDIST_FOLDER`, its
own directory, or the SDK path baked in at configure time. It never links NDI
and never installs it into the system path. The configure step copies the
runtime next to the built helper, so packaging ships the two together. The
application build does not include the helper unless `OPENSHOW_ENABLE_NDI=ON`
and the SDK is found.

## Verified so far (2026-11-07)

- The SDK acquisition script installed the 6.3.2 release to the gitignored
  `third_party/ndi-sdk` (194 MB, including every architecture directory).
- `ndi-helper --probe` loads the runtime and reports
  `NDI SDK LINUX ... 6.3.2.0`, helper `0.1.0`, protocol `1`.
- The real receiver asks for `UYVY` at the source's own frame rate with
  `allow_video_fields = false` (progressive frames). Audio frames are ignored
  and never captured. The first frame decides the ring dimensions; a later
  format or size change is refused rather than reinterpreted.
- **Real source acceptance:** an `IPHONE 7A94 (HX Camera)` at `192.168.0.232`
  is listed and opened in about 0.3–0.5 s. It streams **1920x1080 UYVY, stride
  3840, 60 fps**; 120–180 frames arrive in 2–3 s, the copied frame decodes to a
  real camera picture, `status` reports it open, and `close` unlinks the
  segment. The ring is 12.4 MB instead of the 24.9 MB a BGRA feed needed.
- **UYVY to NV12:** `convertUyvyToNv12` deinterleaves luma and averages the two
  chroma rows (odd last row averaged with itself), keeping stride padding out.
  `live_yuv_tests` pin the pixels and the refusals. An SSSE3 path (with a scalar
  fallback) converts 1080p in 0.15 ms synthetic and 0.31 ms in the real
  pipeline, about 2% of one core at 60 fps. libswscale's UYVY→NV12 was measured
  at 1.0–1.4 ms here, so the specialised loop is retained. This is the handoff
  to the existing `GpuYuvUploader`, the same Skia/Vulkan path FFmpeg playback
  uses; RADV reports no features for `B8G8R8G8_422_UNORM` on this GPU, so a
  native 4:2:2 sample is not an option here.
- **mDNS was the first surprise:** the venue Wi-Fi passes unicast but drops
  multicast between clients. Avahi saw every other mDNS device yet no
  `_ndi._tcp`; a unicast QU query found the phone, and
  `p_extra_ips`/`--extra-ips` made discovery and open work. This is an operator
  setting, not a code workaround.
- **HX was the second:** the phone sends NDI|HX, which the runtime decodes with
  FFmpeg 7.1. Fedora/RPM Fusion only ship FFmpeg 9, so the helper loads a
  minimal 7.1 build (`scripts/fetch-ndi-ffmpeg.sh`) staged beside `libndi.so.6`
  and preloaded with `RTLD_GLOBAL`. Without it the runtime's placeholder reads
  "Video decoder not found"; with it the camera picture decodes at 1080p60.
- The fake backend drives the protocol and frame ring in CI: JSON commands,
  stdout JSON-only, a published POSIX segment, newest-wins reads and a clean
  exit on `quit` or EOF.
- `frame_ring_tests` and `live_source_tests` pin the worker side (ND.3): the
  shared layout, Frame/NoFrame/Error, newest-wins, a real `/dev/shm` segment,
  and `HelperBackend` driving the MIT helper's `--fake` mode over real pipes,
  including a missing executable and an immediately exiting helper. The same
  `HelperBackend` was run against the real phone source through the POSIX
  process boundary and read 180+ frames in 3 s.

ND.1 is accepted. ND.4's remaining acceptance step is the sender disappearing
while open: the helper must emit a typed event and keep running.

## Tests

The camera is needed only for the final manual acceptance. Every layer below it
has a fake, so CI never depends on NDI, a licence, a LAN or a camera.

| Layer | Test | Camera? |
|---|---|---|
| Helper protocol/ring | `helper_unit_tests`, `helper_process_roundtrip` | no |
| Worker live core | `live_source_tests` (worker fake + test helper binary) | no |
| UYVY→NV12 | `live_yuv_tests` | no |
| ND.5 camera asset | domain/storage round trip and migration | no |
| ND.6 background/renderer | headless scene + fake frames | no |
| ND.7 Media Bin/RPC | QML/worker with the fake helper | no |
| ND.8 crash/retry | controlled fake helper | no |
| Real acceptance | `ndi_accept.py`, `ndi_cpu.py`, manual | yes |

- `helper_unit_tests` (fake backend, no SDK): JSON parse/escape, frame-ring
  layout, write/read, newest-wins, a slot still being written, corrupt headers,
  probe/list/status/open/close, malformed and oversized commands, version
  refusal, stdout JSON-only, shutdown on stdin EOF, a real mapped `/dev/shm`
  segment, the UYVY fake frame shape, and a fake `source-lost` event.
- `helper_process_roundtrip`: the real helper binary with `--fake`, checking
  exit status, JSON-only stdout, the published segment name, silent stderr, and
  a UYVY `open` reply (`format`/`stride`).
- `helper_has_no_ndi_link`: `ldd` proves the helper does not link the NDI
  runtime in either build.
- `live_source_tests`: worker `FakeBackend` and `HelperBackend` against the
  test helper binary, including the helper's `source-lost` event and the
  helper's survival afterwards.
- Document/UI tests (ND.5/ND.7): camera asset round trip and storage
  migration, Media Bin action hidden without capability, source picker,
  camera background compiles, placeholder when the source disappears.

### Fake hooks

The fake backend never initialises NDI, so a real camera attached to the
machine cannot affect these tests and tests cannot open the operator's camera:

- `--fake-format uyvy` emits the real 4:2:2 shape (2 bytes/pixel); the default
  remains BGRA for the older tests.
- `--fake-static` freezes the pattern for screenshot/snapshot tests.
- `--fake-lose-source-ms N` emits `source-lost` after N ms, which covers the
  event path deterministically instead of asking someone to unplug a camera.

`HelperBackend` resolves `OPENSHOW_NDI_HELPER` before `ndi-helper` on `PATH`,
so ND.7 tests point the worker at the test helper binary and never at the real
one. Tests must assert on the fake names/format/sequence, never on a real
source name or an exact source count.

Real NDI acceptance is manual/gated: a camera on the LAN, `--list` shows it,
opening streams 1080p frames, the audience shows the live feed, and preview/
audience stay responsive. The SDK cannot be downloaded in CI, so this is not a
default CI gate.

## Known limits (v1)

- Video only; NDI audio is ignored.
- Camera as a slide background only; not a free-positioned foreground item.
- One receiver per open source; no failover source, no PTZ control.
- The helper is launched on demand and stopped when idle; the first frame waits
  for discovery/connection.
- Windows support follows Linux: named shared memory and a helper build against
  the NDI Windows SDK. The protocol and document model are platform-neutral.
