# OpenShowQt

A Linux presentation application for lyrics, slides, images and video, in the
shape of FreeShow. FreeShow itself is Electron; this is a native rebuild with a
new architecture, not a fork.

The operator interface is C++ and Qt 6 Quick/QML. Output runs in a separate
Qt-free worker process: Skia over Vulkan for composition, FFmpeg for decoding,
GLFW for the output window and PipeWire for audio. The frontend owns documents
and compiles them into immutable render scenes, which it sends to the worker as
JSON-RPC over a Unix-domain socket. Preview frames go through shared memory
instead, so a slow UI never blocks the audience output.

The first release targets service playlists, lyrics, a basic visual editor,
images, video, audio, operator previews and one audience display. Stage
outputs, FreeShow file compatibility, Windows and macOS are deliberately out of
scope.

Nothing is usable yet. [PLAN.md](PLAN.md) holds the accepted architecture and
milestones; [PROGRESS.md](PROGRESS.md) records where the work actually stands;
[docs/reference.md](docs/reference.md) documents the FreeShow behaviour we are
reproducing.

## Build environment

The current development machine runs Fedora 46 KDE Plasma (Rawhide/prerelease),
on x86_64 with an AMD Radeon 890M. The versions below were recorded on
**24 October 2026**; they describe the verified installation, rather than strict
minimum versions for every dependency.

| Component | Installed version / configuration |
|---|---|
| GCC C++ | 16.2.1; application uses C++20 |
| CMake / Ninja | 4.3.0 / 1.13.2 |
| Git / Python | 2.56.0 / 3.15.0 |
| Qt | 6.11.2; Core, Gui, Quick/QML and Network |
| Boost | 1.90.0; Boost.JSON |
| GLFW | 3.4.0; Wayland and X11 support |
| Vulkan loader / system headers | 1.4.357 |
| Mesa | 26.2.5; radeonsi for VA-API, RADV for Vulkan |
| PipeWire | 1.6.9 |
| FFmpeg | 9.0.2; libavcodec/libavformat 63.1.102, libavutil 61.1.102, libswscale 10.1.102, libswresample 7.1.102 |
| Fontconfig / HarfBuzz | 2.18.3 / 14.5.1 |
| ICU development headers | 78.3; see the Skia compatibility note below |
| Skia | `a94df1cdabb03d3ead2486413bc2999b500aa4f8`; `third_party/skia` submodule, built by `scripts/build-skia.sh` |
| vk-bootstrap | `v1.3.225`; fetched by CMake |

Use **CMake 3.25 or newer** for the commands below: `CMakePresets.json` uses
preset schema version 6. The project's `cmake_minimum_required` is 3.24, but
that version cannot read these presets.

## Install Fedora dependencies

Enable the **RPM Fusion Free** repository appropriate to your Fedora release
using the [RPM Fusion configuration instructions](https://rpmfusion.org/Configuration).
On Rawhide, follow its development-release instructions. This setup uses the
full `ffmpeg` and `ffmpeg-devel` packages from RPM Fusion, rather than Fedora's
restricted `ffmpeg-free` build. If replacing an existing `ffmpeg-free`
installation, use the following and review the proposed package transaction:

```sh
sudo dnf swap ffmpeg-free ffmpeg --allowerasing
```

Install the build tools and dependencies from the project directory:

```sh
sudo dnf install \
  gcc-c++ git python3 cmake ninja-build pkgconf-pkg-config \
  qt6-qtbase-devel qt6-qtdeclarative-devel \
  glfw-devel boost-devel vulkan-loader-devel vulkan-headers \
  pipewire-devel ffmpeg ffmpeg-devel \
  fontconfig-devel harfbuzz-devel libicu-devel
```

The `headless` preset does not need the two Qt development packages. It still
builds the worker with Skia, Vulkan, GLFW, FFmpeg and PipeWire; it is not a
CPU-only build. `libicu-devel` is a genuine link dependency of the worker
(its Unicode backend is Skia's ICU-backed module, built against the system
ICU) and is also needed to compile that module when building Skia.

For this AMD machine, runtime graphics, audio and sample-scene fonts are
provided by:

```sh
sudo dnf install mesa-dri-drivers mesa-vulkan-drivers pipewire liberation-sans-fonts
```

A graphical Wayland or X11 session is needed for the UI and output-window
checks. Audio playback needs a running user PipeWire session. Other GPU vendors
need their corresponding Vulkan drivers.

Optional diagnostic tools:

```sh
sudo dnf install vulkan-tools vulkan-validation-layers libva-utils
```

The application does not require a separate shader compiler: Skia includes its
own SkSL-to-SPIR-V compiler. Boost.JSON is already the project's JSON library;
no additional JSON or networking framework is required.

```sh
git clone --recursive https://github.com/dattrax/OpenShowQt.git
```

(`git submodule update --init --recursive` is the equivalent for an existing
checkout.) Skia arrives as the `third_party/skia` submodule pinned to the
revision in the table above.

## Build Skia

The first `cmake --preset desktop` (or `headless`) notices that the submodule
has no libraries yet and builds it automatically through
`scripts/build-skia.sh`: it syncs Skia's pinned DEPS git repositories
(`tools/git-sync-deps`), obtains GN (`bin/fetch-gn` unless a `gn` is already
available), and runs GN/ninja with the validated arguments:

```gn
skia_use_vulkan = true
skia_use_gl = false
is_official_build = true
skia_use_libwebp_decode = false
skia_use_libwebp_encode = false
skia_use_system_libpng = false
skia_use_system_freetype2 = false
skia_use_system_libjpeg_turbo = false
skia_use_system_zlib = false
skia_use_system_expat = false
skia_use_system_icu = true
```

PNG, JPEG, FreeType, zlib and Expat are compiled from Skia's bundled copies;
WebP is disabled; Unicode data comes from the **system ICU** (task 6.2b), so
`libicu-devel` must be installed. Building libskia from scratch takes a while
but happens once; later configures and rebuilds find the libraries and skip
straight past it. Run the script yourself to watch it directly, check a tree
without building (`scripts/build-skia.sh --check`), or point it elsewhere:

```sh
scripts/build-skia.sh                                  # the submodule
scripts/build-skia.sh /path/to/skia /path/to/skia/out/Release
```

The application needs `libskia.a`, `libskparagraph.a`, `libskshaper.a`,
`libskunicode_core.a` and `libskunicode_icu.a` in the output directory;
configure fails with instructions if any are missing. It defines `SK_GANESH`,
`SK_VULKAN` and `SK_USE_INTERNAL_VULKAN_HEADERS` when consuming these
libraries. Skia's embedded Vulkan header version can differ from the installed
system headers reported in the table above.

## Configure, build and run

Run the following commands from the OpenShowQt directory. Both presets default
to `RelWithDebInfo` and enable tests. The first configuration downloads
vk-bootstrap `v1.3.225` through CMake FetchContent, so it needs Git and network
access unless that dependency has already been populated.

### Desktop: frontend and worker

```sh
cmake --preset desktop
cmake --build --preset desktop
ctest --preset desktop
```

The resulting executables are:

```sh
./build/src/worker/openshow-worker --check-deps
./build/src/worker/openshow-worker-dev --check-deps
./build/src/ui/openshow-ui
```

The frontend starts the matching worker automatically. The desktop test
suite includes output-window checks and needs a graphical session. The
development harness `openshow-worker-dev` carries the standalone drivers
(`--render-test`, `--snapshot`, `--check-*`, `--pw-check`, `--av-bench`,
`--list-monitors`); the product `openshow-worker` only serves the frontend
over `--socket`. For a brief standalone rendering check on monitor 0:

```sh
./build/src/worker/openshow-worker-dev --render-test --monitor 0 --seconds 5
```

### Headless: worker and shared libraries, without Qt

```sh
cmake --preset headless
cmake --build --preset headless
ctest --preset headless
./build-headless/src/worker/openshow-worker --check-deps
```

The headless test preset excludes tests labelled `display`. GPU tests still
need access to a suitable Vulkan device and can report **Skipped** when one is
unavailable. Media tests generate fixtures with the `ffmpeg` CLI, including
`libx264` and AAC encoding; they skip if fixture generation is unavailable.
Check skipped-test output before treating a run as full verification.

To inspect the selected tests without running them:

```sh
ctest --preset desktop --show-only
ctest --preset headless --show-only
```

### Use a different Skia location

`SKIA_ROOT` resolves in order: the `-DSKIA_ROOT` option, the `$SKIA_ROOT`
environment variable, the `third_party/skia` submodule, and finally the
legacy `../skia` sibling directory. `SKIA_BUILD_DIR` defaults to
`${SKIA_ROOT}/out/Release`. Explicit CMake values take precedence. For
example:

```sh
cmake --preset desktop \
  -DSKIA_ROOT=/absolute/path/to/skia \
  -DSKIA_BUILD_DIR=/absolute/path/to/skia/out/Release
```

The same options work with the headless preset. A tree named this way is used
exactly as it stands — automatic building only ever applies to the submodule,
and `-DOPENSHOW_SKIA_AUTO_BUILD=OFF` skips even that (configure then fails
with instructions instead). Keep headers and libraries from the same Skia
revision and configuration.

## Video decode and GPU composition

`--av-bench FILE` defaults to software decoding directly into host-mapped
Vulkan linear images through FFmpeg's `get_buffer2` allocator. There is no
post-decode CPU frame copy or conversion. Native planar 8-bit YUV420, NV12,
and P010 are supported when the decoder honors custom buffers and the Vulkan
device supplies compatible plane layouts. Each published frame uses one
`vkCmdCopyImage()` (all plane regions) into an optimal image, borrowed once
per presentation slot by Skia with its Vulkan YCbCr sampler.

Decoder references use a separate pool capped at 64 linear images, with four
codec threads and four presentation slots. Decoder images prefer CPU-cached,
coherent host memory because software codecs repeatedly read reference pictures;
uncached memory is used only when no compatible cached type exists. The copying
compatibility path keeps its staging-memory policy. Allocations include decoder
padding; only visible pixels are copied. Images remain alive until both FFmpeg and the
copy fence release them. Host access uses `PREINITIALIZED`/`GENERAL` layouts;
semaphores order copies and Skia sampling, including slot reuse.

Unsupported decoders, native formats (including low-bit-aligned planar 10-bit
YUV), or device layouts fail explicitly. To permit a copying compatibility
fallback during initial setup:

```sh
build/src/worker/openshow-worker-dev --av-bench /path/to/video.mp4 --allow-video-copy
```

This reopens the file using VA-API downloads when available, or software
plane copies/conversion. `--no-hw-decode` disables VA-API in that fallback;
it does not change the direct software path. Resolution or pixel-format
changes after playback starts are terminal errors. The benchmark reports
`direct`, `downloads`, `CPU-copies`, `conversions`, `vkCmdCopyImage`, and
`decoder-images` / `cached-decoder-images` counts. Direct mode keeps downloads,
CPU copies, and conversions at zero; cached decoder images should match total
decoder images on devices offering a compatible cached, coherent memory type. Preview capture and test readbacks are separate from video ingestion.

`gpu_decode_tests` checks real H.264/HEVC decoding, B-frame order, cropping,
reference lifetimes, looping, all three native plane layouts, pool exhaustion,
and explicit High10 compatibility conversion. Enable Vulkan synchronization
validation with:

```sh
OPENSHOW_VULKAN_VALIDATION=1 \
VK_LAYER_VALIDATE_SYNC=1 \
ctest --test-dir build-headless -R 'gpu_(decode|yuv)_tests' --output-on-failure
```

For an API capture, run `./gpu_decode_tests --capture-direct` from
`build-headless/tests` under a Vulkan capture layer. This mode decodes
24 H.264 frames; the capture should contain 24 three-plane `vkCmdCopyImage`
commands, with no extra video upload or RGB conversion image.

## Optional FFmpeg hardware decoding in compatibility mode

The installed FFmpeg 9.0.2 already supports VA-API; no additional FFmpeg build
is needed. Hardware decode is used only by the explicitly permitted copying
fallback, because downloading its surfaces does not meet the direct-path
contract.

On this AMD/Fedora setup, H.264 and HEVC hardware decoding need the Freeworld
Mesa codec packages. For VA-API:

```sh
sudo dnf install mesa-va-drivers-freeworld.x86_64
vainfo --display drm --device /dev/dri/renderD128
```

`VAEntrypointVLD` entries indicate decoding support for the corresponding
profiles. Use the render node belonging to your GPU if it is not
`/dev/dri/renderD128`. Keep the Freeworld packages compatible with the installed
Mesa version. See [Fedora's hardware video acceleration guidance](https://fedoraproject.org/wiki/Hardware_Video_Acceleration).

For Vulkan Video decoding, the separate optional package and capability check
are:

```sh
sudo dnf install mesa-vulkan-drivers-freeworld.x86_64
vulkaninfo | grep -E 'VK_KHR_video_(decode|queue)'
```

`ffmpeg -hwaccels` lists mechanisms compiled into FFmpeg, rather than proving
that a specific codec can run on your GPU. Test an actual H.264 video with:

```sh
ffmpeg -hide_banner -nostdin \
  -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 \
  -hwaccel_output_format vaapi -i /path/to/h264-video.mp4 \
  -map 0:v:0 -an -frames:v 60 \
  -vf 'hwdownload,format=nv12' -f null -
```

This test downloads GPU frames as NV12 and discards the output. The download
filter requires hardware frames, so a silent fallback to software frames does
not count as a successful test. This example targets ordinary 8-bit H.264;
other bit depths can require a different download format.

As of 24 October 2026 on this machine:

- `mesa-va-drivers-freeworld` **26.2.5-1.fc46.x86_64** is installed.
- A 60-frame H.264 High-profile, 1920x1080 VA-API decoding test passed after
  installing it. VP9 and AV1 hardware decoding also passed earlier short tests.
- The driver advertises HEVC Main and Main10 decode profiles; HEVC playback
  has not yet been tested.
- `mesa-vulkan-drivers-freeworld` is not installed, and H.264/HEVC Vulkan
  decoding has not been verified.

These driver checks do not select an application decode path. OpenShowQt logs
its effective decoder; direct mode uses software decoding, while the optional
compatibility fallback may use VA-API.
