add_executable(domain_tests
  domain_tests.cpp
)
target_link_libraries(domain_tests PRIVATE openshow::domain)
add_test(NAME domain_tests COMMAND domain_tests)

add_executable(scene_tests
  scene_tests.cpp
)
target_link_libraries(scene_tests PRIVATE openshow::scene)
add_test(NAME scene_tests COMMAND scene_tests)

add_executable(protocol_tests
  protocol_tests.cpp
)
target_link_libraries(protocol_tests PRIVATE openshow::protocol)
add_test(NAME protocol_tests COMMAND protocol_tests)

# The worker runs as its own process and must never gain a Qt dependency.
add_test(NAME worker_is_qt_free
  COMMAND ${CMAKE_COMMAND}
          -DTARGET=$<TARGET_FILE:openshow-worker>
          -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckNoQt.cmake
)

# Task 6.2b (review R9): application and test code use public Skia APIs
# only — no includes of Skia's private/ or src/ trees (modules/<m>/src/
# included). The exemption list is empty since the ICU-backed Unicode
# module retired SceneText.cpp's private includes.
add_test(NAME skia_public_api_guard
  COMMAND ${CMAKE_COMMAND}
          -DROOT=${CMAKE_SOURCE_DIR}
          -DSRC_DIRS=src\;tests
          -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckNoSkiaPrivate.cmake
)

# Inverted, with a fixture that does include a private header, so the
# guard above is proven not to scan nothing.
add_test(NAME skia_public_api_guard_negative
  COMMAND ${CMAKE_COMMAND}
          -DROOT=${CMAKE_CURRENT_SOURCE_DIR}/fixtures
          -DSRC_DIRS="skia_private"
          -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckNoSkiaPrivate.cmake
)
set_tests_properties(skia_public_api_guard_negative PROPERTIES WILL_FAIL TRUE)

# The same check, inverted, proves the guard is not vacuous.
if(OPENSHOW_BUILD_UI)
  add_test(NAME ui_links_qt
    COMMAND ${CMAKE_COMMAND}
            -DTARGET=$<TARGET_FILE:openshow-ui>
            -DCHECK=linked
            -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckNoQt.cmake
  )


  # Task 3.3: DocumentController, the document session behind the lyric
  # panel (editing, debounced text, navigation, compile hand-off, service
  # save/load, autosave startup rule, undo). Qt Core only - no display -
  # but Qt arrives with the UI switch, so this follows ui_links_qt.
  add_executable(document_controller_tests document_controller_tests.cpp)
  target_link_libraries(document_controller_tests PRIVATE openshow::ui-core)
  add_test(NAME document_controller_tests COMMAND document_controller_tests)

  # Task 3.4's clear transforms and content flags, pinned headless: the
  # derived scenes must survive the strict wire validator and remove
  # exactly one layer each (Qt Core only, like the controller fixtures).
  add_executable(scene_edit_tests scene_edit_tests.cpp)
  target_link_libraries(scene_edit_tests PRIVATE openshow::ui-core)
  add_test(NAME scene_edit_tests COMMAND scene_edit_tests)

  # Task 4.1's panel resize: the reference's snap boundaries and the
  # persistence rules, pinned headless against a temp workspace.ini.
  add_executable(workspace_panels_tests workspace_panels_tests.cpp)
  target_link_libraries(workspace_panels_tests PRIVATE openshow::ui-core)
  add_test(NAME workspace_panels_tests COMMAND workspace_panels_tests)

  # Task 4.2's drawer search: the reference's normalisation, the
  # every-word-at-a-word-start contract and the confidence tiers.
  add_executable(library_search_tests library_search_tests.cpp)
  target_link_libraries(library_search_tests PRIVATE openshow::ui-core)
  add_test(NAME library_search_tests COMMAND library_search_tests)

  # Task 6.3a (review R3): WorkerController correlates replies by request id
  # and connection generation, mirrors each prepare's own payload, presents
  # only the latest go, and resets every intent when the worker dies. The
  # binary re-executes itself as a scripted RPC peer that is slow, refuses
  # and dies on cue — no real worker, no display.
  add_executable(worker_controller_tests worker_controller_tests.cpp)
  target_link_libraries(worker_controller_tests PRIVATE
    openshow::ui-core openshow::protocol)
  add_test(NAME worker_controller_tests COMMAND worker_controller_tests)
  set_tests_properties(worker_controller_tests PROPERTIES TIMEOUT 120)

  # Task 3.3's acceptance sentence, end to end: DocumentController creates
  # and types a song, and the compiled slides are prepared and presented
  # on a real openshow-worker, status confirming each revision on air.
  # Needs a display, like the rpc lifecycle scenario.
  add_executable(document_present_tests document_present_tests.cpp)
  target_link_libraries(document_present_tests PRIVATE openshow::ui-core openshow::protocol)
  add_test(NAME document_present_tests
    COMMAND document_present_tests $<TARGET_FILE:openshow-worker>)
  set_tests_properties(document_present_tests PROPERTIES LABELS display TIMEOUT 90)
endif()

# Runs the Qt-free worker with no display and no audio session.
add_test(NAME worker_check_deps COMMAND openshow-worker --check-deps)

# The task 1.1 scene composed on the CPU with no display, window or Vulkan.
add_test(NAME render_scene_snapshot
  COMMAND openshow-worker --snapshot ${CMAKE_CURRENT_BINARY_DIR}/test-scene.png
)

# The output layer model (PLAN.md): disabled layers dispatch nothing to Skia
# and enabled ones paint only inside their region. CPU only.
add_test(NAME render_layer_composition
  COMMAND openshow-worker --check-layers
)

# Task 3.2 transitions: the fade's media (800ms) and text (500ms) tracks,
# completion, cut, and cut-cancels-fade, timed from drawn frames through
# the production layers. CPU only.
add_test(NAME render_scene_transition
  COMMAND openshow-worker --check-transition
)

# Task 6.1a (review R1): the production compositor profile — uniform black
# start incl. the footer region, clock/aux off, scene composition identical
# to the proven demo path, and the demo picture intact when asked for.
# CPU only.
add_test(NAME render_production_compositor
  COMMAND openshow-worker --check-production
)

# Task 3.1 scene typography: production layout (wrap, centring, fallback,
# shrink-to-fit) rendered on the CPU through the same engine the audience
# words layer uses. No display or Vulkan; exits 77 when no usable fonts.
add_executable(scene_render_tests scene_render_tests.cpp)
target_link_libraries(scene_render_tests PRIVATE openshow::worker_render)
set_source_files_properties(scene_render_tests.cpp PROPERTIES
  COMPILE_OPTIONS "-fno-rtti"
)
add_test(NAME scene_render_tests COMMAND scene_render_tests)
set_tests_properties(scene_render_tests PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)

# Task 6.2b (review R9): the public ICU-backed SkUnicode module — grapheme
# clustering (combining marks, emoji ZWJ/flag sequences), UAX #14 CJK
# wrapping, UBA bidi regions for mixed Hebrew/Latin, working break
# iterators — through the production text engine. Skips (77) without fonts.
add_executable(unicode_text_tests unicode_text_tests.cpp)
target_link_libraries(unicode_text_tests PRIVATE openshow::worker_render)
set_source_files_properties(unicode_text_tests.cpp PROPERTIES
  COMPILE_OPTIONS "-fno-rtti"
)
add_test(NAME unicode_text_tests COMMAND unicode_text_tests)
set_tests_properties(unicode_text_tests PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)

# Task 1.5 media foundation, linked straight into the Qt-free test binary.
# Fixtures are generated with the ffmpeg CLI; the test exits 77 (skipped)
# when the CLI is not available to produce them.
add_executable(media_tests media_tests.cpp)
target_link_libraries(media_tests PRIVATE openshow::worker_media)
add_test(NAME media_tests COMMAND media_tests)
set_tests_properties(media_tests PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)

# Task 1.5d/1.5i: NV12 slot write + commit -> wrapped ycbcr SkImage -> GPU
# draw -> readback, pixel-compared against the sampler's own conversion
# matrix, plus the FrameBacking contract the ring drives. Needs a Vulkan GPU
# but no display server; skips (77) when the device or context is missing.
add_executable(gpu_yuv_tests gpu_yuv_tests.cpp)
target_link_libraries(gpu_yuv_tests PRIVATE openshow::worker_gpu)
add_test(NAME gpu_yuv_tests COMMAND gpu_yuv_tests)
set_tests_properties(gpu_yuv_tests PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)

# Task 1.2 end-to-end: spawn a real openshow-worker --socket, handshake,
# change output state live, and take both exits (served shutdown and the
# frontend going away). It opens a window, so it carries the `display`
# label, which the headless test preset excludes.
add_executable(rpc_ipc_tests rpc_ipc_tests.cpp)
target_link_libraries(rpc_ipc_tests PRIVATE openshow::protocol openshow::scene)
add_test(NAME worker_rpc_lifecycle
  COMMAND rpc_ipc_tests $<TARGET_FILE:openshow-worker>)
set_tests_properties(worker_rpc_lifecycle PROPERTIES LABELS display TIMEOUT 120)

# Task 5.5 acceptance: background video, preload and media visibility
# over a live --socket session: lyrics preserve playback (the media
# position climbs through a lyric change over the same background), a
# missing/corrupt background produces a media.error notification and a
# sticky status error that recovery clears, and prepared slides reach the
# air within 100 ms p95. Fixtures are generated with the ffmpeg CLI
# (skips 77 without it); the worker opens a window, so it is desktop.
add_executable(background_media_tests background_media_tests.cpp)
target_link_libraries(background_media_tests PRIVATE openshow::protocol openshow::scene)
add_test(NAME background_media_tests
  COMMAND background_media_tests $<TARGET_FILE:openshow-worker>)
set_tests_properties(background_media_tests PROPERTIES LABELS display TIMEOUT 240)
# Task 1.5e acceptance: the event-driven scheduler. --idle-check watches the
# render loop from a second thread and fails (exit 1) if a static scene
# presents anything, burns more than 5% CPU over the window, or ignores a
# layer toggle. Needs a display like the rpc scenario.
add_test(NAME render_idle_check
  COMMAND openshow-worker --render-test --idle-check)
set_tests_properties(render_idle_check PROPERTIES LABELS display TIMEOUT 60)

# The M3 checkpoint demo caught the event-driven loop sleeping through a
# crossfade (one frame at blend 0, then quiet: the audience never saw the
# new scene). This pins the live contract from the frontend's side: frame
# counts climb across a fade and plateau after it, and the preview stream
# settles on the new scene. Needs a display; Qt-free.
add_executable(live_fade_tests live_fade_tests.cpp)
target_link_libraries(live_fade_tests PRIVATE openshow::protocol)
add_test(NAME live_fade_tests
  COMMAND live_fade_tests $<TARGET_FILE:openshow-worker>)
set_tests_properties(live_fade_tests PROPERTIES LABELS display TIMEOUT 90)

# Task 6.1a (review R1): the production composition rules over a real
# --socket session: the fresh worker's live preview is uniformly black
# incl. the footer band, output.status reports the production layer
# defaults, a presented scene (colour + words) carries no diagnostics in
# the footer, and clear-all returns to uniform black. Needs a display.
add_executable(production_output_tests production_output_tests.cpp)
target_link_libraries(production_output_tests PRIVATE openshow::protocol)
add_test(NAME production_output_tests
  COMMAND production_output_tests $<TARGET_FILE:openshow-worker>)
set_tests_properties(production_output_tests PROPERTIES LABELS display TIMEOUT 90)

# Task 6.4 (review R4): output.status publishes only presented frames.
# Injected fault knobs force the two windows R4 named — presents that
# fail while a scene is queued, and a command landing mid scene-apply —
# and the fixtures read the live status stream and preview tags against
# them. Needs a display.
add_executable(output_status_tests output_status_tests.cpp)
target_link_libraries(output_status_tests PRIVATE openshow::protocol)
add_test(NAME output_status_tests
  COMMAND output_status_tests $<TARGET_FILE:openshow-worker>)
set_tests_properties(output_status_tests PROPERTIES LABELS display TIMEOUT 120)

# Real FFmpeg get_buffer2 allocations into Vulkan linear images, independent
# reference/presentation lifetimes, all native plane layouts and strict errors.
add_executable(gpu_decode_tests gpu_decode_tests.cpp)
target_link_libraries(gpu_decode_tests PRIVATE openshow::worker_gpu openshow::worker_media PkgConfig::FFMPEG)
add_test(NAME gpu_decode_tests COMMAND gpu_decode_tests)
set_tests_properties(gpu_decode_tests PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)

# Task 5.1: the production bounded decode queue (PlaybackPipeline). CPU-mode
# scenarios pin the ring high-water and the frozen-clock park with no GPU;
# GPU scenarios run the same pipeline over a headless device against a stuck
# presenter. Skips (77) without the ffmpeg CLI or a Vulkan device.
add_executable(playback_queue_tests playback_queue_tests.cpp)
target_link_libraries(playback_queue_tests PRIVATE
  openshow::worker_playback
  openshow::worker_gpu
)
add_test(NAME playback_queue_tests COMMAND playback_queue_tests)
set_tests_properties(playback_queue_tests PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)

# Task 5.2: playback controls — pause/resume freeze and re-anchor, forward
# and backward seek flushing through the decode thread and pacing seam, a
# parked seek, and the live loop toggle. CPU mode against the wall clock;
# skips (77) without the ffmpeg CLI.
add_executable(playback_control_tests playback_control_tests.cpp)
target_link_libraries(playback_control_tests PRIVATE
  openshow::worker_playback
  openshow::worker_gpu
)
add_test(NAME playback_control_tests COMMAND playback_control_tests)
set_tests_properties(playback_control_tests PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)

# Task 5.3: the production AudioMixer — bounded per-input rings, independent
# volume/mute with a clickless ramp, starvation accounting — against a fake
# output device, plus real AudioFileFeeders decoding locally written sine
# wavs (no CLI dependency; no environment skip applies).
add_executable(audio_mix_tests audio_mix_tests.cpp)
target_link_libraries(audio_mix_tests PRIVATE
  openshow::worker_media
  PkgConfig::FFMPEG
)
add_test(NAME audio_mix_tests COMMAND audio_mix_tests)
set_tests_properties(audio_mix_tests PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)

# Task 5.4: playback clocks and synchronization — AudioClock transport
# primitives, a real-chain feeder seek (feeder -> mixer -> paced fake
# device over an internally written pattern wav: the stale queue is
# flushed and the target region is heard), AvPlayer on the monotonic
# fallback, and AvPlayer's live transport (pause holds, play continues,
# seek rebases) against the PipeWire daemon — the live scenario reports
# an honest skip when there is no daemon.
add_executable(av_sync_tests av_sync_tests.cpp)
target_link_libraries(av_sync_tests PRIVATE
  openshow::worker_playback
  openshow::worker_media
  PkgConfig::FFMPEG
)
add_test(NAME av_sync_tests COMMAND av_sync_tests)
set_tests_properties(av_sync_tests PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
