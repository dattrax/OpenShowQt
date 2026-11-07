# openshow-ndi-helper - the separate, MIT-licensed NDI process.
#
# This is a standalone project, not part of the application's CMake build. It
# exists so the GPL application never links the proprietary NDI SDK; see
# ../../docs/ndi-camera-input.md.
#
# With the SDK (the default):
#   cmake -S tools/ndi-helper -B build-ndi-helper \
#     -DOPENSHOW_NDI_SDK_DIR="$PWD/third_party/ndi-sdk"
#
# Fake backend only (CI, and any machine without the SDK):
#   cmake -S tools/ndi-helper -B build-ndi-helper-fake \
#     -DOPENSHOW_NDI_HELPER_WITH_NDI=OFF

cmake_minimum_required(VERSION 3.24)

project(openshow-ndi-helper VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(OPENSHOW_NDI_HELPER_WITH_NDI
  "Build the NDI backend (needs OPENSHOW_NDI_SDK_DIR)" ON)
option(OPENSHOW_NDI_HELPER_TESTS "Build the helper's tests" ON)

set(NDI_SOURCES)
set(NDI_DEFINITIONS)
set(NDI_INCLUDE_DIRS)
set(NDI_RUNTIME_FILE "")

if(OPENSHOW_NDI_HELPER_WITH_NDI)
  if(NOT OPENSHOW_NDI_SDK_DIR)
    message(FATAL_ERROR
      "OPENSHOW_NDI_HELPER_WITH_NDI is ON but OPENSHOW_NDI_SDK_DIR is not set.\n"
      "Download the NDI SDK after accepting its licence, then run\n"
      "  scripts/fetch-ndi-sdk.sh /path/to/Install_NDI_SDK_v6_Linux.tar.gz\n"
      "and configure with -DOPENSHOW_NDI_SDK_DIR=<repo>/third_party/ndi-sdk.\n"
      "For a backend-only build without NDI pass -DOPENSHOW_NDI_HELPER_WITH_NDI=OFF.")
  endif()

  get_filename_component(OPENSHOW_NDI_SDK_DIR "${OPENSHOW_NDI_SDK_DIR}" ABSOLUTE)
  if(NOT EXISTS "${OPENSHOW_NDI_SDK_DIR}/include/Processing.NDI.Lib.h")
    message(FATAL_ERROR
      "'${OPENSHOW_NDI_SDK_DIR}' does not look like the NDI SDK "
      "(missing include/Processing.NDI.Lib.h)")
  endif()

  # The runtime is loaded with dlopen/LoadLibrary, never linked. Pick the
  # architecture directory that matches this build, then fall back to whichever
  # single runtime the SDK ships.
  set(NDI_ARCH_DIR "")
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    set(NDI_ARCH_DIR "x86_64-linux-gnu")
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(NDI_ARCH_DIR "aarch64-rpi4-linux-gnueabi")
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(i[3-6]86|x86)$")
    set(NDI_ARCH_DIR "i686-linux-gnu")
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^arm")
    set(NDI_ARCH_DIR "arm-rpi4-linux-gnueabihf")
  endif()

  set(NDI_RUNTIME_CANDIDATES)
  if(NDI_ARCH_DIR)
    list(APPEND NDI_RUNTIME_CANDIDATES
      "${OPENSHOW_NDI_SDK_DIR}/lib/${NDI_ARCH_DIR}/libndi.so.6"
      "${OPENSHOW_NDI_SDK_DIR}/bin/${NDI_ARCH_DIR}/libndi.so.6")
  endif()
  file(GLOB NDI_RUNTIME_FALLBACKS
    "${OPENSHOW_NDI_SDK_DIR}/lib/*/libndi.so.6"
    "${OPENSHOW_NDI_SDK_DIR}/bin/*/libndi.so.6")
  list(APPEND NDI_RUNTIME_CANDIDATES ${NDI_RUNTIME_FALLBACKS})

  foreach(candidate ${NDI_RUNTIME_CANDIDATES})
    if(EXISTS "${candidate}")
      get_filename_component(NDI_RUNTIME_FILE "${candidate}" REALPATH)
      get_filename_component(NDI_RUNTIME_DIR "${candidate}" DIRECTORY)
      break()
    endif()
  endforeach()

  if(NOT NDI_RUNTIME_FILE)
    message(WARNING
      "No libndi.so.6 found under '${OPENSHOW_NDI_SDK_DIR}'; the helper will "
      "still build but needs OPENSHOW_NDI_RUNTIME or the runtime beside it.")
    set(NDI_RUNTIME_DIR "${OPENSHOW_NDI_SDK_DIR}/bin")
  endif()

  # NDI|HX decoding happens inside the NDI runtime through FFmpeg 7.1
  # (libavcodec.so.61 / libavutil.so.59). A modern system FFmpeg is usually a
  # newer, incompatible major, so stage a 7.1 build beside the helper and let
  # it preload those libraries. Full-bandwidth NDI needs none of this.
  set(OPENSHOW_NDI_FFMPEG_DIR "${OPENSHOW_NDI_FFMPEG_DIR}" CACHE PATH
    "Directory with FFmpeg 7.1 shared libraries for NDI|HX decoding")
  if(NOT OPENSHOW_NDI_FFMPEG_DIR)
    set(_ndiHxDefault
      "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/ffmpeg-7.1/lib")
    if(EXISTS "${_ndiHxDefault}/libavcodec.so.61")
      set(OPENSHOW_NDI_FFMPEG_DIR "${_ndiHxDefault}" CACHE PATH
        "Directory with FFmpeg 7.1 shared libraries for NDI|HX decoding" FORCE)
    endif()
  endif()
  set(NDI_HX_LIBRARIES)
  if(OPENSHOW_NDI_FFMPEG_DIR)
    foreach(soname libavcodec.so.61 libavutil.so.59 libswscale.so.8
                   libswresample.so.5)
      if(EXISTS "${OPENSHOW_NDI_FFMPEG_DIR}/${soname}")
        list(APPEND NDI_HX_LIBRARIES "${OPENSHOW_NDI_FFMPEG_DIR}/${soname}")
      endif()
    endforeach()
  endif()

  list(APPEND NDI_SOURCES src/ndi_backend.cpp)
  list(APPEND NDI_DEFINITIONS
    OPENSHOW_NDI_HELPER_WITH_NDI
    OPENSHOW_NDI_RUNTIME_DIR="${NDI_RUNTIME_DIR}")
  list(APPEND NDI_INCLUDE_DIRS "${OPENSHOW_NDI_SDK_DIR}/include")
endif()

set(HELPER_SOURCES
  src/main.cpp
  src/protocol.cpp
  src/json.cpp
  src/shm_ring.cpp
  src/fake_backend.cpp
  ${NDI_SOURCES}
)

add_executable(ndi-helper ${HELPER_SOURCES})

target_include_directories(ndi-helper PRIVATE
  src
  "${CMAKE_CURRENT_SOURCE_DIR}/../../shared"
  ${NDI_INCLUDE_DIRS}
)
target_compile_definitions(ndi-helper PRIVATE ${NDI_DEFINITIONS})

if(WIN32)
  target_link_libraries(ndi-helper PRIVATE ws2_32)
else()
  target_link_libraries(ndi-helper PRIVATE pthread)
endif()

if(NDI_RUNTIME_FILE)
  # Packaging: the runtime sits beside the helper and is found by
  # executableDir()'s candidate list, so no system install is needed.
  add_custom_command(TARGET ndi-helper POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${NDI_RUNTIME_FILE}"
            "$<TARGET_FILE_DIR:ndi-helper>/libndi.so.6"
    COMMENT "Copying the NDI runtime beside the helper")
endif()

if(NDI_SOURCES)
  # The helper preloads the staged FFmpeg 7.1 libraries by soname; $ORIGIN is
  # what makes its own directory part of that search. libndi.so.6 already
  # carries the same runpath for the same reason.
  set_target_properties(ndi-helper PROPERTIES
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN")
endif()

set(NDI_HX_COPY_COMMANDS)
foreach(library ${NDI_HX_LIBRARIES})
  get_filename_component(real "${library}" REALPATH)
  get_filename_component(name "${library}" NAME)
  list(APPEND NDI_HX_COPY_COMMANDS
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${real}"
            "$<TARGET_FILE_DIR:ndi-helper>/${name}")
endforeach()
if(NDI_HX_COPY_COMMANDS)
  add_custom_command(TARGET ndi-helper POST_BUILD ${NDI_HX_COPY_COMMANDS}
    COMMENT "Copying the FFmpeg 7.1 libraries for NDI|HX beside the helper")
elseif(NDI_SOURCES)
  message(STATUS
    "NDI|HX decoding: no FFmpeg 7.1 libraries staged, so only full NDI "
    "sources will decode (see scripts/fetch-ndi-ffmpeg.sh)")
endif()

if(OPENSHOW_NDI_HELPER_TESTS)
  enable_testing()

  add_executable(helper_tests
    tests/helper_tests.cpp
    src/protocol.cpp
    src/json.cpp
    src/shm_ring.cpp
    src/fake_backend.cpp
  )
  target_include_directories(helper_tests PRIVATE
    src
    "${CMAKE_CURRENT_SOURCE_DIR}/../../shared"
  )
  if(WIN32)
    target_link_libraries(helper_tests PRIVATE ws2_32)
  else()
    target_link_libraries(helper_tests PRIVATE pthread)
  endif()

  add_test(NAME helper_unit_tests COMMAND helper_tests)

  # The executable protocol round trip: a real process, commands on stdin and
  # JSON-only stdout. The fake backend means no SDK and no camera are needed.
  add_test(NAME helper_process_roundtrip
    COMMAND ${CMAKE_COMMAND}
            -DBINARY=$<TARGET_FILE:ndi-helper>
            -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/process_roundtrip.cmake
  )

  # ND.0's licence boundary, applied to the helper: the helper loads NDI with
  # dlopen and never links it, in any build.
  add_test(NAME helper_has_no_ndi_link
    COMMAND ${CMAKE_COMMAND}
            -DBINARY=$<TARGET_FILE:ndi-helper>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_no_ndi_link.cmake
  )
endif()
