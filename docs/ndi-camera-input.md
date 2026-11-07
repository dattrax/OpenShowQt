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

- The worker owns the generic `LiveSourceHub` and `LiveFrameStore`. It launches
  the helper on demand, sends control commands and maps the frame ring. It
  contains no NDI headers or symbols; a CI guard checks the app binary.
- The helper owns NDI discovery and receiving. It is a separate executable with
  its own repository/submodule, licence and NDI attribution. It can be omitted
  entirely; the app still builds and runs.
- The worker's `HelperBackend` and a `FakeBackend` implement the same
  `LiveSourceBackend` interface. The fake backend is always built for tests; the
  helper backend is generic IPC and part of the GPL app.
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
- The worker's background transport detects the `ndi://` asset path and asks the
  live hub for frames instead of opening a file. The renderer draws the newest
  frame like a video background. Previews use the same path, so the operator
  sees the camera off-air too.
- The source name is stored in the document; the worker resolves it to a live
  receiver. If the source is missing, the document still opens and the slide
  shows a placeholder.

## SDK acquisition and build

The NDI SDK is **not** committed and **not** a git submodule. Download it from
<https://ndi.video/> after accepting the NDI SDK licence, then run:

```sh
scripts/fetch-ndi-sdk.sh /path/to/Install_NDI_SDK_v6_Linux.tar.gz
```

The script extracts the SDK to `third_party/ndi-sdk` (gitignored) and prints the
`OPENSHOW_NDI_SDK_DIR` to use. It also works with `NDI_SDK_ARCHIVE` and refuses
to overwrite an existing SDK without `--force`.

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
- `ndi-helper --list` returns an empty source list on a network with no NDI
  sender, and `ldd` shows no `libndi` dependency.
- The fake backend drives the protocol and frame ring in CI: JSON commands,
  stdout JSON-only, a published POSIX segment, newest-wins reads and a clean
  exit on `quit` or EOF.
- The real receiver requests `BGRX_BGRA` at the source's own frame rate with
  `allow_video_fields = false` (progressive frames). Audio frames are ignored
  and never captured. The first frame decides the ring dimensions, so a source
  that changes resolution is reopened rather than resized in place.

Still outstanding for ND.1/ND.4 acceptance: naming a real camera on the LAN and
streaming its frames into the ring. That needs the operator's camera on the
network and is recorded here when it is run.

## Tests

- `helper_unit_tests` (fake backend, no SDK): JSON parse/escape, frame-ring
  layout, write/read, newest-wins, a slot still being written, corrupt headers,
  probe/list/status/open/close, malformed and oversized commands, version
  refusal, stdout JSON-only and shutdown on stdin EOF; plus a real mapped
  `/dev/shm` segment.
- `helper_process_roundtrip`: the real helper binary with `--fake`, checking
  exit status, JSON-only stdout, the published segment name and silent stderr.
- `helper_has_no_ndi_link`: `ldd` proves the helper does not link the NDI
  runtime in either build.
- Worker tests (fake helper, ND.3/ND.7): capability advertisement, source list,
  open/close, shm mapping, newest-frame handoff, helper crash → typed fault, no
  NDI symbols in the application binary.
- Document/UI tests: camera asset round trip and storage migration, Media Bin
  action hidden without capability, source picker, camera background compiles,
  placeholder when the source disappears.
- Real NDI acceptance is manual/gated: a camera on the LAN, `--list` shows it,
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
