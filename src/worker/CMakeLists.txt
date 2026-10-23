add_executable(openshow-worker
  main.cpp
  output/OutputWindow.cpp
  output/Swapchain.cpp
  output/VulkanContext.cpp
  preview/PreviewStream.cpp
  render/Layer.cpp
  render/TestScene.cpp
  rpc/RpcServer.cpp
  session/OutputSession.cpp
)
# The prebuilt Skia is compiled with RTTI off; TestScene derives from its
# classes, whose typeinfo does not exist, so this TU must also be -fno-rtti.
set_source_files_properties(render/TestScene.cpp PROPERTIES
  COMPILE_OPTIONS "-fno-rtti"
)
add_executable(openshow::worker ALIAS openshow-worker)

# The task 1.5 media layer (probe, then frame ring, resampler, YUV upload).
# A static library so tests can link it directly; the public headers speak
# plain C++ (FFmpeg.h is module-private), which also keeps libav includes
# out of everything else. PUBLIC include dir = src/worker for "media/..." paths.
add_library(openshow-worker-media STATIC
  media/MediaProbe.cpp
)
add_library(openshow::worker_media ALIAS openshow-worker-media)
target_link_libraries(openshow-worker-media PRIVATE PkgConfig::FFMPEG)
target_include_directories(openshow-worker-media PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# The worker must stay free of Qt. tests/CheckNoQt.cmake enforces this.
target_link_libraries(openshow-worker PRIVATE
  openshow::domain
  openshow::protocol
  openshow::skia
  openshow::worker_media
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

# shm_open lives in libc since glibc 2.34 and in librt before that.
find_library(OPENSHOW_LIBRT rt)
if(OPENSHOW_LIBRT)
  target_link_libraries(openshow-worker PRIVATE ${OPENSHOW_LIBRT})
endif()
