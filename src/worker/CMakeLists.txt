add_executable(openshow-worker
  main.cpp
  output/OutputWindow.cpp
  output/Swapchain.cpp
  output/VulkanContext.cpp
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

# The worker must stay free of Qt. tests/CheckNoQt.cmake enforces this.
target_link_libraries(openshow-worker PRIVATE
  openshow::domain
  openshow::protocol
  openshow::skia
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
