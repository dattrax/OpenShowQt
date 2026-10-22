# OpenShowQt - Linux presentation application.
#
# Three build products:
#   openshow-domain    Qt-independent document and protocol library
#   openshow-worker    Qt-free output process (GLFW, Skia/Vulkan, FFmpeg, PipeWire)
#   openshow-ui        Qt 6 Quick frontend
#
# Prerequisites on Fedora:
#   qt6-qtbase-devel qt6-qtdeclarative-devel glfw-devel pipewire-devel
#   ffmpeg-devel boost-devel vulkan-devel
# Plus a Skia tree built with Vulkan, see cmake/OpenShowSkia.cmake.

cmake_minimum_required(VERSION 3.24)

project(OpenShowQt VERSION 0.1.0 LANGUAGES CXX)

# FindBoost is gone from CMake 4; use the Boost config package everywhere.
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(OPENSHOW_BUILD_UI "Build the Qt 6 Quick frontend" ON)
option(OPENSHOW_BUILD_TESTS "Build tests" ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GLFW REQUIRED IMPORTED_TARGET GLOBAL glfw3)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET GLOBAL libpipewire-0.3)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET GLOBAL
  libavcodec libavformat libavutil libswscale libswresample)
# libskia references fontconfig symbols when the font manager is used, and
# libskshaper's harfbuzz backend references the system harfbuzz.
pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET GLOBAL fontconfig)
pkg_check_modules(HARFBUZZ REQUIRED IMPORTED_TARGET GLOBAL harfbuzz)

find_package(Vulkan REQUIRED)
find_package(Threads REQUIRED)
find_package(Boost REQUIRED COMPONENTS json)

include(cmake/OpenShowSkia.cmake)

include(FetchContent)
FetchContent_Declare(vk_bootstrap
  GIT_REPOSITORY https://github.com/charles-lunarg/vk-bootstrap
  GIT_TAG v1.3.225
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(vk_bootstrap)

if(OPENSHOW_BUILD_UI)
  find_package(Qt6 REQUIRED COMPONENTS Core Gui Quick Network)
  qt_standard_project_setup()
endif()

add_subdirectory(src/domain)
add_subdirectory(src/protocol)
add_subdirectory(src/worker)

if(OPENSHOW_BUILD_UI)
  add_subdirectory(src/ui)
endif()

if(OPENSHOW_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

message(STATUS "OpenShowQt ${CMAKE_PROJECT_VERSION}")
message(STATUS "  Skia:        ${OPENSHOW_SKIA_LIBRARY}")
message(STATUS "  GLFW:        ${GLFW_VERSION}")
message(STATUS "  PipeWire:    ${PIPEWIRE_VERSION}")
execute_process(COMMAND ${PKG_CONFIG_EXECUTABLE} --modversion libavcodec
  OUTPUT_VARIABLE FFMPEG_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE)
message(STATUS "  FFmpeg:      ${FFMPEG_VERSION} (libav*)")
message(STATUS "  Boost:       ${Boost_VERSION}")
message(STATUS "  Qt frontend: ${OPENSHOW_BUILD_UI}")
