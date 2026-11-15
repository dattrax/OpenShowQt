# Design conformance review — 2026-11-15

Reviewed against commit `054a1d0` and the current `PLAN.md`. The working tree
was clean at the start. This review changes documentation only.

The original architecture is recognisable and worth retaining: Qt owns the
editable documents; domain and compiled scenes are separate libraries; the
Qt-free worker owns audience rendering, media, audio and shared-memory preview
production. The main problems found are incomplete integration of newer media
types into older control and persistence paths, and new code bypassing existing
threading and transport contracts. A rewrite is not indicated.

This is a targeted source and regression review, not an exhaustive audit of all
features. It concentrates on presentation/clear/restore, save-time asset adoption,
audio staging, Library preparation and Media Bin RPC. Prior review closures stay
historical evidence; the findings below are new follow-ups.

## Findings

### D1 — P1: Clear Background fails for video and camera backgrounds

Sources: [SceneEdit.cpp](../../src/ui/SceneEdit.cpp#L41),
[scene.cpp](../../src/scene/scene.cpp#L1029),
[PresentationModel.cpp](../../src/ui/PresentationModel.cpp#L203).

`dropBackground()` clears `image`, the asset reference, colour and gradient,
but retains `video`, `camera`, `cameraSource` and `cameraMirror`.

- For a video, the resulting scene has `video:true` and `image:false`. The
  codec refuses it with `background 'video' requires 'image'`, so the clear
  cannot reach the worker.
- For a camera, the codec sees `camera:true` and reinstates `hasImage` and the
  camera source. The clear is accepted but still contains the camera. Meanwhile
  `sceneContent()` reads the edited JSON's `image:false` and reports no
  background. The audience and the operator's indicator can disagree.

A temporary C++ probe linked to the production UI/scene libraries reproduced
both cases by serialising a scene, calling `withBackgroundCleared()`, and
validating the result through `scene::fromJson()`:

```text
CLEAR video: valid=0 image=0 camera=0 UI-background=0
  error=scene: background 'video' requires 'image'
CLEAR camera: valid=1 image=1 camera=1 UI-background=0
```

**Correction:** construct a complete, valid cleared background through the
typed scene contract, including kind/source flags, while preserving foreground
items and the separately authored audio layer. Derive indicators from the same
normalised interpretation used by the worker. Cover still/video/camera/gradient
and their combinations with words and show audio; verify acknowledged pixels
and camera retirement, then Restore. Existing `scene_edit_tests` exercise still
and gradient clears but miss these video/camera cases.

### D2 — P1: save-time path adoption misses show audio

Sources: [PresentationModel.cpp](../../src/ui/PresentationModel.cpp#L265),
[DocumentPersistence.cpp](../../src/ui/DocumentPersistence.cpp#L82),
[scene.cpp](../../src/scene/scene.cpp#L879).

`adoptAssetPaths()` rewrites `background.assetPath` and image items but never
the new top-level `audio.assetPath`. Its pending prepare intents, desired
presentation, revision mirror and Restore snapshot therefore retain the old
audio path after a save. The saved package can be correct while this running
frontend still holds stale scene content. Clearing/restoring after old source
bytes are removed can fail; a save can itself remove those bytes when they
belong to the recovery package being superseded.

The production-model probe presented an audio-only scene, adopted an old → new
path map, then issued Clear All/Restore. Both the pending prepare and Restore
still named `/tmp/old-autosave/assets/track.wav`, rather than the saved-service
path.

**Correction:** centralise traversal of every scene asset reference and reuse
it for all retained payloads. Add save → remove old bytes → clear → restore
coverage for audio-only and video-plus-audio cues, including saves while prepare
is pending. Assert actual re-open/playback and that adoption alone does not
re-present or restart the live cue.

### D3 — P1: audio-only cues perform blocking bring-up on the frame thread

Sources: [OutputSession.cpp](../../src/worker/session/OutputSession.cpp#L391),
[BackgroundTransport.cpp](../../src/worker/session/BackgroundTransport.cpp#L1884),
[AvPlayer.cpp](../../src/worker/playback/AvPlayer.cpp#L55).

`preloadScene()` returns before audio staging for backgrounds with no image,
for stills, and for cameras. `primeAudio()` is tied to the video-file prime.
Consequently a show audio layer on any of those backgrounds is probed and
started inside `applyBackground()` on the render thread. `AvPlayer::start()`
can wait for PipeWire negotiation, file opens and pre-roll. This reopens the
scheduling problem that 6.21c/6.22e were intended to close.

The existing on-air-video shortcut also compares only video ID/path. Adding or
changing the audio layer on that video skips preparation although the eventual
transport identity includes the audio layer, so that change also opens cold.

A production `BackgroundTransport` probe used a generated two-second WAV and
the existing `OPENSHOW_FAULT_AUDIO_START_MS=700` / matching-path fault seam:

```text
AUDIO-ONLY applyBackground: result=0 elapsed=722ms
```

PipeWire was unavailable to the sandbox, so this proves blocking of the calling
thread, not audible latency or physical display performance. The synthetic
delay is supported by the real synchronous start/probe call chain.

**Correction:** stage an audio-capable transport bundle independently of whether
a video exists, keyed by the complete intended media identity. Carry probes,
duration, source identities and failures with it. On a missing/not-ready bundle,
defer the scene with bounded latest-wins ownership; do not fall back to a
synchronous file open on the presentation thread. Test audio-only,
still-plus-audio, camera-plus-audio and same-video/new-audio under slow start,
replacement, cancellation and failure.

### D4 — P1: staged audio source flags cross threads without their mutex

Sources: [BackgroundTransport.cpp](../../src/worker/session/BackgroundTransport.cpp#L1619),
[BackgroundTransport.cpp](../../src/worker/session/BackgroundTransport.cpp#L1805),
[BackgroundTransport.cpp](../../src/worker/session/BackgroundTransport.cpp#L1878).

`primeAudio()` writes `fPrimedEmbeddedAudio` and `fPrimedLayerAudio` under
`fPrimeMutex` on the prepare executor. The render thread moves the staged
player and some metadata under that mutex, releases it, and later reads and
clears these two plain `bool` fields outside the mutex. Another prepare can
publish the next bundle between those operations. This is a C++ data race;
it can also assign the next bundle's source flags to the adopted player or
erase them before that next bundle is adopted.

This finding is source-confirmed; no ThreadSanitizer result is claimed.

**Correction:** move the player and all associated metadata into local ownership
in one critical section, preferably as one staged-bundle value. Do not solve
this by making the flags individually atomic: they must remain paired with the
player they describe. Use a deterministic barrier test that publishes B after
A is taken but before A is activated; check both players' source identities.
Run that CPU handoff under ThreadSanitizer where supported.

### D5 — P2: an audio-only cue can be labelled clear with Clear All disabled

Sources: [SceneEdit.cpp](../../src/ui/SceneEdit.cpp#L62),
[OutputPanel.qml](../../src/ui/OutputPanel.qml#L266),
[ShortcutMap.qml](../../src/ui/ShortcutMap.qml#L100).

On an initially black output, a cue consisting only of the new show audio
layer has `wordsOnAir=false`, `backgroundOnAir=false` and no Restore point.
The audience panel labels this "Output clear"; both its Clear All button and
the Escape clear/restore shortcut are disabled. The transport can be playing
audible content, but the normal clear action is unavailable. Pause and master
mute remain possible, which does not make the clear state correct.

The scene-content probe confirmed both visual flags are false for an audio-only
scene. The disabled controls and label follow directly from their QML gates.

**Correction:** represent acknowledged cue audio activity separately from visual
composition layers. Use it in overall output activity and Clear All/Escape
availability. Keep words/background clears scoped to their own content. Test
audio-only Go → acknowledged active state → Clear All → silence → Restore,
including paused and muted cues and cues arriving while Go is pending.

### D6 — P2: Library edits can be lost while a linked song is preparing

Sources: [ServiceCatalogBridge.cpp](../../src/ui/ServiceCatalogBridge.cpp#L31),
[ServiceCatalogBridge.cpp](../../src/ui/ServiceCatalogBridge.cpp#L126),
[ServiceCatalogBridge.cpp](../../src/ui/ServiceCatalogBridge.cpp#L176).

Preparation captures stanzas/metadata/title at `start()`. An edit arriving
while the planner is busy writes its ID to `fDeferredRefresh`. On completion,
the bridge only refreshes that deferred ID if it differs from the ID just
prepared. Editing the same song during its preparation therefore publishes
the old snapshot and discards the request for its newer contents. The initial
add can miss the notification altogether because no linked service show yet
exists. Changes to several other songs also overwrite one another in the
single deferred slot. The catalog can be current while linked service lyrics
remain stale indefinitely.

This finding is source-confirmed; no delayed-worker integration run is claimed.

**Correction:** capture and recheck catalog content/record revisions plus the
destination service generation before publishing. Maintain a deduplicated set
of dirty linked IDs bounded by catalog size, and process every affected ID;
same-ID edits require another run. Cancel removed sources and obsolete service
targets, and drain/retry pending work after failures. Test same-item edits during
initial Add and Refresh, changes to multiple linked songs, metadata-only edits,
deletion and service replacement while a measurement is delayed.

### D7 — P2: Media Bin thumbnails bypass the control/pixel separation

Sources: [RpcMethods.cpp](../../src/worker/rpc/RpcMethods.cpp#L655),
[RpcMethods.cpp](../../src/worker/rpc/RpcMethods.cpp#L705),
[VideoThumbnail.cpp](../../src/worker/media/VideoThumbnail.cpp#L98).

`media.describe` probes files synchronously in RPC dispatch.
`media.thumbnail` synchronously opens/demuxes/decodes a video, converts a frame,
encodes PNG, and returns its base64 bytes in JSON. This bypasses both the
existing deferred-work mechanism and the fixed architecture's rule that RPC
carries metadata/paths while pixels use shared memory. MB.6's recorded
base64 implementation conflicts with that fixed contract.

The 128-video-packet cap does not bound time spent opening the input, finding
stream info or reading intervening packets. One slow request can hold up later
Go, clear, transport and status requests on the same RPC connection. This does
not imply the independently running audience frame loop always stalls.

**Correction:** retain the Media Bin feature, but put probe/decode work on a
bounded cancellable executor with deferred replies and request generations.
Deliver pixels through the existing preview mechanism or a worker-produced
cache file referenced by path. Apply byte/time budgets and finite input
validation; prove a delayed/failed thumbnail cannot delay control requests,
grow queues indefinitely or install a stale card after reconnection.

## Design and planning corrections

The implementation has accumulated changes beyond the original FreeShow-like
core. The operator amendments, static PowerPoint import and camera **input**
are documented features; they should not be removed merely because an older
scope paragraph defers NDI **output**. Likewise, preserve the latest audio cue
workflow rather than restoring the removed standalone-track controls from an
older DONE row.

The audio contract does need one explicit reconciliation. The fixed architecture
and older architecture narrative describe an independent standalone track;
the latest implementation adds a show-owned track mixed with video in a single
player, while the older standalone RPC remains. The show track currently
restarts when the background identity changes, even if the track is unchanged.
Decide and record continuity across background changes, show boundaries,
Clear Background, seek/loop and Restore. Recommended default: retain the latest
cue workflow and make a show's unchanged track continue across its slides and
visual-only changes; deliberate retrigger/seek must be explicit. If the intended
policy differs, record it before refactoring transport ownership. This is a
design decision, not a claim of measured drift between two mixer inputs.

Already-open work is distinct from these regressions: TV.3/TV.4a/TV.4b/TV.5,
TA.1, ND.4's real source-loss acceptance, and M7.1a–c/7.2–7.4 remain open. In
particular worker recovery, display removal safety, device-loss handling,
packaging and long physical rehearsals cannot be inferred from earlier DONE
milestones or a green unit suite. Complete the correction group and the
existing release gates before enabling FO's additional outputs.

`PLAN.md` and `PROGRESS.md` also need maintenance after this review: the plan
contains very long completed-task narratives despite its own concise-ledger
rule; the progress file contains superseded next actions and test/schema counts.
Preserve their evidence in history and leave one current handoff. Treat counts
as historical unless they are accompanied by a fresh command and result.

## Correction sequence and acceptance

The authoritative task/status ledger is the new **DR** group in `PLAN.md`.
Each implementation leaf should stay independently reviewable, generally
0.5–2 engineer-days; split it further if its prototype exceeds that size.

1. **DR.0:** record audio continuity/clear semantics and one current contract.
2. **DR.1, DR.2, DR.4:** fix incorrect clears, stale asset paths and the staged
   metadata race first; add regressions that fail against this reviewed commit.
3. **DR.3, DR.5:** finish the audio staging path and operator activity/clear
   integration against DR.0, using DR.4's complete handoff bundle.
4. **DR.6, DR.7a, DR.7b:** make Library refresh lossless and separate Media Bin
   background work/pixels from the control connection.
5. **DR.8:** run integrated journeys plus current desktop/headless suites and
   source guards; report skips and physical gates explicitly; update the
   current handoff and archive stale narratives. Then resume the existing
   TV/TA/ND/M7 work in dependency order.

Do not reclassify D1–D7 as merely M7 hardware checks. Most have deterministic
CPU/model/IPC regressions and should close before another broad feature push.

## Verification performed

- `CCACHE_DIR=/tmp/openshow-review-ccache cmake --build --preset desktop -j 2`
  passed. The first attempt stopped because ccache tried to write outside the
  sandbox; moving only its cache to `/tmp` resolved the environmental failure.
- 18 focused existing tests passed (18.09 seconds): documents, catalog,
  scenes, scene editing/frame/store, protocol, transport, prepare executor,
  preview stream/producer, controller, lyric/pagination/text flow and audio mix.
  The green existing suite does not cover the failing probes above.
- All 10 architecture/dependency guards passed: Qt-free worker/development
  worker, Qt-linked UI, public Skia APIs, rendering-layer/target rules and
  absence of NDI SDK symbols, including negative guard fixtures (1.37 seconds).
- `ctest --test-dir build --output-on-failure -LE display`: 63 passed and
  `worker_controller_tests` timed out (302.88 seconds total). Its local scripted
  peer could not listen in the sandbox. Re-running that exact test outside the
  sandbox passed in 60.34 seconds. Thus every one of the 64 selected test
  entries passed in a usable environment, but the first suite run was not
  green. The 21 display-labelled entries were not run.
- The broader run included workspace/control-set/UI loading, raster and GPU
  rendering/decode, fonts, playback, live-source seams and PowerPoint checks.
  Live PipeWire transport/restart/sink-restart/lane-state branches reported
  internal skips because connection was denied; VA-API was unavailable and
  software fallback was exercised. Passing entries do not close those hardware
  acceptance gaps. `git diff --check` and local review-link checks passed.
- Temporary production-library probes reproduced D1, D2, D3 and D5 as described.
  They were built outside the repository; no product implementation or tests
  were modified. D4, D6 and D7 are source findings with explicit regression
  work specified above.
- No projector, Wayland/X11 hotplug, live camera unplug, audible pairing or
  long-service acceptance was performed. PipeWire access was unavailable to
  the sandboxed audio probe; no audible result is claimed.
