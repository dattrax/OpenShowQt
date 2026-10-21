add_executable(openshow-worker
  main.cpp
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
  vk-bootstrap::vk-bootstrap
)
