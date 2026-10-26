add_executable(openshow-worker
  main.cpp
  output/OutputWindow.cpp
  output/Swapchain.cpp
  render/TestScene.cpp
  rpc/RpcServer.cpp
  session/OutputSession.cpp
)

# The shared-memory preview producer (task 1.4). A library so the slot
# ownership rules (task 6.5, review R5) can be pinned headless against
# the real object — the worker links it like its other layers.
add_library(openshow-worker-preview STATIC
  preview/PreviewStream.cpp
)
add_library(openshow::worker_preview ALIAS openshow-worker-preview)
target_link_libraries(openshow-worker-preview PUBLIC openshow::protocol)
target_include_directories(openshow-worker-preview PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# shm_open lives in libc since glibc 2.34 and in librt before that.
find_library(OPENSHOW_LIBRT rt)
if(OPENSHOW_LIBRT)
  target_link_libraries(openshow-worker-preview PUBLIC ${OPENSHOW_LIBRT})
endif()

# The outbound queue (task 6.8b, review R11) is header-only, and its rules —
# which frames may be replaced, which may be dropped, when the queue says no —
# are worth pinning headless against the real object rather than only through a
# live socket. This carries the worker's include root and the protocol limit its
# byte budget is taken from.
add_library(openshow-worker-rpc-headers INTERFACE)
target_include_directories(openshow-worker-rpc-headers INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(openshow-worker-rpc-headers INTERFACE openshow::protocol)
add_library(openshow::worker_rpc_headers ALIAS openshow-worker-rpc-headers)

# The output layer model, the production compositor (task 6.1b) and scene
# text (task 3.1). A library so the headless fixture tests can lay out and
# render scenes without the worker's main(), window or Vulkan. The
# development harness (render/TestScene.cpp, the demo decoration and the
# pixel checks) stays out of it: production code does not depend on the
# demo, only the CLI drives both.
add_library(openshow-worker-render STATIC
  render/Layer.cpp
  render/SceneRenderer.cpp
  render/SceneText.cpp
)
add_library(openshow::worker_render ALIAS openshow-worker-render)
target_link_libraries(openshow-worker-render PUBLIC
  openshow::scene
  openshow::skia
  PkgConfig::FONTCONFIG
  PkgConfig::HARFBUZZ
)
target_include_directories(openshow-worker-render PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# The prebuilt Skia is compiled with RTTI off; SceneText derives from its
# classes, whose typeinfo does not exist, so this TU must also be -fno-rtti.
set_source_files_properties(render/SceneText.cpp PROPERTIES
  COMPILE_OPTIONS "-fno-rtti"
)

# Vulkan device setup and the YUV GPU upload ring (task 1.5d). A library so
# the headless GPU test can link it without the worker's main().
add_library(openshow-worker-gpu STATIC
  output/VulkanContext.cpp
  output/VulkanMemoryAdapter.cpp
  render/YuvGpu.cpp
)
add_library(openshow::worker_gpu ALIAS openshow-worker-gpu)
target_link_libraries(openshow-worker-gpu PUBLIC
  openshow::skia
  PkgConfig::GLFW
  vk-bootstrap::vk-bootstrap
)
target_include_directories(openshow-worker-gpu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# The prebuilt Skia is compiled with RTTI off; this TU builds SkParagraph
# content whose classes carry no typeinfo, so it must also be -fno-rtti.
set_source_files_properties(render/TestScene.cpp PROPERTIES
  COMPILE_OPTIONS "-fno-rtti"
)
add_executable(openshow::worker ALIAS openshow-worker)

# The task 1.5 media layer (probe, then frame ring, resampler, YUV upload).
# A static library so tests can link it directly; the public headers speak
# plain C++ (FFmpeg.h is module-private), which also keeps libav includes
# out of everything else. PUBLIC include dir = src/worker for "media/..." paths.
add_library(openshow-worker-media STATIC
  media/AudioFeeder.cpp
  media/AudioMixer.cpp
  media/AudioRing.cpp
  media/FrameRing.cpp
  media/MediaProbe.cpp
  media/PipeWireSink.cpp
  media/SeekQueue.cpp
  media/VideoFrameSource.cpp
)
add_library(openshow::worker_media ALIAS openshow-worker-media)
target_link_libraries(openshow-worker-media PRIVATE PkgConfig::FFMPEG PkgConfig::PIPEWIRE)
target_include_directories(openshow-worker-media PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Task 5.1: the production decode→present pipeline (bounded queues, paced
# submission, the session's presenter role). A library so the queue-bound
# tests drive the very object --av-bench uses, in GPU and CPU modes, with
# neither the worker's main() nor a window.
add_library(openshow-worker-playback STATIC
  playback/PlaybackPipeline.cpp
  playback/AvPlayer.cpp
)
add_library(openshow::worker_playback ALIAS openshow-worker-playback)
target_link_libraries(openshow-worker-playback PUBLIC
  openshow::worker_gpu
  openshow::worker_media
)
target_include_directories(openshow-worker-playback PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# The worker must stay free of Qt. tests/CheckNoQt.cmake enforces this.
target_link_libraries(openshow-worker PRIVATE
  openshow::domain
  openshow::protocol
  openshow::scene
  openshow::skia
  openshow::worker_gpu
  openshow::worker_media
  openshow::worker_playback
  openshow::worker_preview
  openshow::worker_render
  PkgConfig::GLFW
  PkgConfig::PIPEWIRE
  PkgConfig::FFMPEG
  PkgConfig::FONTCONFIG
  PkgConfig::HARFBUZZ
  vk-bootstrap::vk-bootstrap
)
target_include_directories(openshow-worker PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)
