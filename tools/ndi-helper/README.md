# openshow-ndi-helper

A small, separate executable that speaks NDI for OpenShowQt. It owns NDI
discovery and receiving so the GPL application never links the proprietary NDI
SDK. The application launches this helper, sends line-delimited JSON on its
stdin and reads JSON on its stdout; video frames arrive through a POSIX
shared-memory ring.

This directory is licensed **MIT** (see `LICENSE`). It is not part of the GPL
application and is not built by the application's default CMake configuration.

## Build

The NDI SDK is proprietary and is not committed. Download it from
<https://ndi.video/> after accepting the NDI SDK licence, then:

```sh
../../scripts/fetch-ndi-sdk.sh /path/to/Install_NDI_SDK_v6_Linux.tar.gz
cmake -S . -B ../../build-ndi-helper \
  -DOPENSHOW_NDI_SDK_DIR="$PWD/../../third_party/ndi-sdk"
cmake --build ../../build-ndi-helper
../../build-ndi-helper/ndi-helper --probe
```

The helper loads the NDI runtime with `dlopen`/`LoadLibrary`
(`libndi.so.6` or `ProcessingNDI.dll`); it never links it. Lookup order is
`OPENSHOW_NDI_RUNTIME`, `NDILIB_REDIST_FOLDER`, the helper's own directory,
the SDK directory baked in at configure time, then the system loader. The
configure step copies the runtime beside the built helper, so packaging can
ship the two together.

Without the SDK (CI and any machine that only needs the protocol), build the
fake backend:

```sh
cmake -S . -B ../../build-ndi-helper-fake -DOPENSHOW_NDI_HELPER_WITH_NDI=OFF
cmake --build ../../build-ndi-helper-fake
ctest --test-dir ../../build-ndi-helper-fake --output-on-failure
```

The fake backend produces a moving 320x180 BGRA colour-bar pattern at 30 fps.
It exercises the protocol, the frame ring and the helper process without NDI or
a camera.

## Command line

| Option | Meaning |
|---|---|
| *(none)* | Server mode: read protocol commands from stdin |
| `--probe` | Print helper, protocol and NDI runtime versions, then exit |
| `--list` | Print discovered sources, then exit |
| `--wait-ms N` | Discovery wait for `--list` (default 1500, capped at 10000) |
| `--fake` | Use the built-in colour-bar source instead of NDI |
| `--input FILE` | Read server-mode commands from FILE (used by the process test) |
| `--version` | Print the helper version |
| `--help` | Print usage |

`--probe` and `--list` print one JSON object on stdout. The fake-only build
refuses to start the NDI backend with a typed error rather than silently
pretending to be NDI.

## Protocol

One JSON object per line. stdout carries protocol objects only; diagnostics go
to stderr. The helper exits on `quit` or when stdin reaches EOF. Commands may
carry `"protocol":1`; any other version is refused without exiting.

| Command | Response |
|---|---|
| `{"cmd":"probe"}` | `{"ok":true,"helper":"<version>","protocol":1,"ndi":"<runtime version>","canList":true,"canReceive":true}` |
| `{"cmd":"list","waitMs":1500}` | `{"ok":true,"sources":[{"name":"...","url":"..."}]}` |
| `{"cmd":"open","source":"..."}` | `{"ok":true,"source":"...","shm":"/openshow-ndi-<pid>-<n>","width":1920,"height":1080,"stride":7680,"format":"BGRA","bytes":...,"fps":30}` |
| `{"cmd":"status"}` | `{"ok":true,"open":true,...same fields while open}` |
| `{"cmd":"close"}` | `{"ok":true}` |
| `{"cmd":"quit"}` | `{"ok":true}` then exit |

Asynchronous events are JSON objects with `"event"` instead of `"ok"`:
`{"event":"source-lost","source":"...","message":"..."}` and
`{"event":"receiver-error","source":"...","message":"..."}`. They are emitted
from the receiver thread and serialised with command replies, so lines never
interleave.

A failed command answers `{"ok":false,"error":"..."}` and leaves the helper
running. Unknown commands, malformed JSON, duplicate keys and command lines
longer than 64 KiB are refused without exiting.

## Frame ring

The ring layout is the MIT header in
`../../shared/openshow/live/frame_ring.h`. A ring has a fixed 128-byte header,
three 64-byte slot headers and three pixel buffers. The helper writes the newest
frame into the next slot and publishes a sequence; the reader copies the newest
slot and retries if the slot's sequence changed underneath it. Newest wins —
there is no growing queue.

Only `open` creates a ring, and its dimensions come from the first NDI frame.
The segment name is unlinked on `close`, on a new `open`, and when the helper
exits. A reader that already mapped the segment keeps its mapping until it
unmaps.

## Tests

`ctest` runs three checks in either build:

- `helper_unit_tests` — JSON parsing and escaping, frame-ring write/read,
  newest-wins, a slot still being written, corrupt headers, the protocol
  against the fake backend (including oversized lines and EOF), and a real
  mapped `/dev/shm` segment.
- `helper_process_roundtrip` — runs the real binary with `--fake`, checks
  stdout is JSON-only, the exit status, a published segment name and silent
  stderr.
- `helper_has_no_ndi_link` — `ldd` proves the helper never links `libndi`.

Real NDI acceptance needs a camera on the LAN and the SDK installed. It is not a
default CI gate because the SDK cannot be downloaded unattended. Recorded
results live in `../../docs/ndi-camera-input.md`.
