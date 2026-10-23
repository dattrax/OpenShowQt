add_executable(domain_tests
  domain_tests.cpp
)
target_link_libraries(domain_tests PRIVATE openshow::domain)
add_test(NAME domain_tests COMMAND domain_tests)

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

# Task 1.5 media foundation, linked straight into the Qt-free test binary.
# Fixtures are generated with the ffmpeg CLI; the test exits 77 (skipped)
# when the CLI is not available to produce them.
add_executable(media_tests media_tests.cpp)
target_link_libraries(media_tests PRIVATE openshow::worker_media)
add_test(NAME media_tests COMMAND media_tests)
set_tests_properties(media_tests PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)

# Task 1.2 end-to-end: spawn a real openshow-worker --socket, handshake,
# change output state live, and take both exits (served shutdown and the
# frontend going away). It opens a window, so it carries the `display`
# label, which the headless test preset excludes.
add_executable(rpc_ipc_tests rpc_ipc_tests.cpp)
target_link_libraries(rpc_ipc_tests PRIVATE openshow::protocol)
add_test(NAME worker_rpc_lifecycle
  COMMAND rpc_ipc_tests $<TARGET_FILE:openshow-worker>)
set_tests_properties(worker_rpc_lifecycle PROPERTIES LABELS display TIMEOUT 90)
