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
