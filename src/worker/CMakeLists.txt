# Task 6.18b2: the production worker and the dev harness drive the same
# OutputSession, so both compile the same session/output sources. What is
# production-only is main.cpp's socket mode and rpc/RpcServer.cpp; what is
# dev-only is the driver main and the demo scene / pixel checks it calls
# (render/TestScene.cpp). The product binary no longer carries them.
add_executable(openshow-worker
  main.cpp
  output/OutputWindow.cpp
  output/Swapchain.cpp
  rpc/RpcServer.cpp
  session/OutputSession.cpp
)

add_executable(openshow-worker-dev
  dev/main.cpp
  output/OutputWindow.cpp
  output/Swapchain.cpp
  render/TestScene.cpp
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

# The output's scene truth (task 6.18a, review R1/R4/R5/R10/R13): the bounded
# prepared store with the ids it mints, the output revision, and the desired /
# landed / published halves of what the audience sees. A library so those rules
# can be pinned headless against the real object rather than only through a live
# worker. It needs no window, no Vulkan and no Skia — which is what moving
# SceneTransitionKind into the layer header bought: a component that owns scene
# truth has no business depending on a GPU library to name a reveal.
add_library(openshow-worker-session STATIC
  session/SceneStore.cpp
)
add_library(openshow::worker_session ALIAS openshow-worker-session)
target_link_libraries(openshow-worker-session PUBLIC openshow::scene)
target_include_directories(openshow-worker-session PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# The audience-side media (task 6.18b, review R6/R10/R15): the background
# transport (the same PlaybackPipeline + AvPlayer pair --av-bench drives), the
# standalone track and its swap, the video seam's bounded slots and their
# leases, the newest-wins intent slots and the whole media status mirror. A
# library so those rules can be pinned headless against the real object rather
# than only through a live worker; it travels with playback — and so with the
# GPU-upload types — but not with a window or the compositor.
add_library(openshow-worker-session-transport STATIC
  session/BackgroundTransport.cpp
)
add_library(openshow::worker_session_transport ALIAS openshow-worker-session-transport)
target_link_libraries(openshow-worker-session-transport PUBLIC
  openshow::worker_playback
  openshow::scene
  PkgConfig::GLFW
)
target_include_directories(openshow-worker-session-transport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# The prepare-time file work (task 6.9a, review R10): one bounded,
# cancellable worker thread for the still decodes and media probes a
# scene.prepare schedules, so the single RPC thread never parks inside a
# cold decode. A library so the exactly-once/cancellation/capacity rules
# can be pinned headless with injectable work, without a session or a disk.
add_library(openshow-worker-session-prepare STATIC
  session/PrepareExecutor.cpp
)
add_library(openshow::worker_session_prepare ALIAS openshow-worker-session-prepare)
target_include_directories(openshow-worker-session-prepare PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# The preview stream's owner (task 6.18a2): the lifecycle of the shm segment,
# the three targets' one-frame-per-pass arbitration, the selection and batch
# generation gates, the audience's cost policy and the thumbnail queue with
# its command-thread decoder. A library so those rules can be pinned headless
# against the real object — it needs no window and no Vulkan, but it draws, so
# unlike SceneStore it does travel with Skia and the compositor.
add_library(openshow-worker-preview-producer STATIC
  session/PreviewProducer.cpp
)
add_library(openshow::worker_preview_producer ALIAS openshow-worker-preview-producer)
target_link_libraries(openshow-worker-preview-producer PUBLIC
  openshow::worker_preview
  openshow::worker_render
  openshow::protocol
)
target_include_directories(openshow-worker-preview-producer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

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

# The worker must stay free of Qt. tests/CheckNoQt.cmake enforces this for
# both binaries.
set(OPENSHOW_WORKER_LIBS
  openshow::domain
  openshow::protocol
  openshow::scene
  openshow::skia
  openshow::worker_gpu
  openshow::worker_media
  openshow::worker_playback
  openshow::worker_preview
  openshow::worker_preview_producer
  openshow::worker_render
  openshow::worker_session
  openshow::worker_session_prepare
  openshow::worker_session_transport
  PkgConfig::GLFW
  PkgConfig::PIPEWIRE
  PkgConfig::FFMPEG
  PkgConfig::FONTCONFIG
  PkgConfig::HARFBUZZ
  vk-bootstrap::vk-bootstrap
)
target_link_libraries(openshow-worker PRIVATE ${OPENSHOW_WORKER_LIBS})
target_include_directories(openshow-worker PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(openshow-worker-dev PRIVATE ${OPENSHOW_WORKER_LIBS})
target_include_directories(openshow-worker-dev PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)
