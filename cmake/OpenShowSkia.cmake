# Defines openshow::skia from a Skia tree built with GN/ninja.
#
# SKIA_ROOT:      Skia source tree, from -DSKIA_ROOT, $SKIA_ROOT, or ../skia
# SKIA_BUILD_DIR: GN output directory, from -DSKIA_BUILD_DIR, $SKIA_BUILD_DIR,
#                 or ${SKIA_ROOT}/out/Release
#
# The application does not compile shaders. Skia's own SkSL to SPIR-V code
# generator is part of libskia, so no glslc, glslang or shaderc is required.

if(NOT SKIA_ROOT AND DEFINED ENV{SKIA_ROOT})
  set(SKIA_ROOT "$ENV{SKIA_ROOT}")
endif()
if(NOT SKIA_ROOT)
  set(SKIA_ROOT "${CMAKE_SOURCE_DIR}/../skia")
endif()

if(NOT SKIA_BUILD_DIR AND DEFINED ENV{SKIA_BUILD_DIR})
  set(SKIA_BUILD_DIR "$ENV{SKIA_BUILD_DIR}")
endif()
if(NOT SKIA_BUILD_DIR)
  set(SKIA_BUILD_DIR "${SKIA_ROOT}/out/Release")
endif()

set(OPENSHOW_SKIA_LIBRARY "")
foreach(candidate libskia.a libskia.so skia.lib)
  if(EXISTS "${SKIA_BUILD_DIR}/${candidate}")
    set(OPENSHOW_SKIA_LIBRARY "${SKIA_BUILD_DIR}/${candidate}")
    break()
  endif()
endforeach()

if(NOT OPENSHOW_SKIA_LIBRARY)
  message(FATAL_ERROR
    "No libskia found in ${SKIA_BUILD_DIR}. Build Skia with Vulkan enabled:\n"
    "  cd ${SKIA_ROOT} && python3 tools/git-sync-deps\n"
    "  bin/gn gen out/Release --args='skia_use_vulkan=true skia_use_gl=false "
    "is_official_build=true'\n"
    "  ninja -C out/Release skia")
endif()

add_library(openshow_skia INTERFACE IMPORTED GLOBAL)
add_library(openshow::skia ALIAS openshow_skia)

# This project is pinned to the Skia revision it was built and validated
# against (task 6.2a): a silently mismatched libskia is the classic
# mystery-crash generator, and the worker's VulkanMemoryAdapter implements
# exactly this revision's public allocator contract. Override with
# -DOPENSHOW_SKIA_REVISION=any only after validating another revision.
set(OPENSHOW_SKIA_REVISION "a94df1cdabb03d3ead2486413bc2999b500aa4f8"
  CACHE STRING "Skia revision SKIA_ROOT must be at ('any' to skip the check)")
if(NOT OPENSHOW_SKIA_REVISION STREQUAL "any")
  execute_process(
    COMMAND git -C "${SKIA_ROOT}" rev-parse HEAD
    OUTPUT_VARIABLE actualSkiaRevision
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE skiaRevisionResult
  )
  if(NOT skiaRevisionResult EQUAL 0)
    message(WARNING
      "SKIA_ROOT (${SKIA_ROOT}) is not a git checkout; cannot verify it is "
      "at Skia ${OPENSHOW_SKIA_REVISION}.")
  elseif(NOT actualSkiaRevision STREQUAL OPENSHOW_SKIA_REVISION)
    message(FATAL_ERROR
      "Skia at ${SKIA_ROOT} is at ${actualSkiaRevision}, but OpenShowQt is "
      "pinned to ${OPENSHOW_SKIA_REVISION}.\n"
      "  git -C ${SKIA_ROOT} checkout ${OPENSHOW_SKIA_REVISION}\n"
      "  ninja -C ${SKIA_BUILD_DIR}\n"
      "or configure with -DOPENSHOW_SKIA_REVISION=any for a revision you "
      "have validated yourself.")
  endif()
endif()

# Public Skia API only (task 6.2a, review R9): include/ plus the root that
# the modules' public headers are reached through. The private trees
# (include/private, src) are deliberately NOT exported; the single
# remaining consumer (SceneText.cpp, SkTArray/SkUTF/hardcoded-Unicode)
# includes them by root-relative path and is listed as a guard exemption
# until task 6.2b retires it. tests/CheckNoSkiaPrivate.cmake enforces the
# rule from the source side.
target_include_directories(openshow_skia SYSTEM INTERFACE
  "${SKIA_ROOT}"
  "${SKIA_ROOT}/include"
  "${SKIA_ROOT}/include/third_party/vulkan"
)

# Must match the defines Skia was built with.
target_compile_definitions(openshow_skia INTERFACE
  SK_VULKAN
  SK_USE_INTERNAL_VULKAN_HEADERS
  SK_GANESH
)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_compile_definitions(openshow_skia INTERFACE SK_BUILD_FOR_UNIX)
endif()

# Text layout: SkParagraph with its shaping and Unicode core. Full ICU is
# deliberately not linked here: libskunicode_icu expects ICU v77 symbols while
# this Fedora ships 78, so the worker uses the hard-coded SkUnicode backend
# for now (recorded in PROGRESS.md as an M3 decision).
set(OPENSHOW_SKIA_TEXT_LIBRARIES "")
foreach(textLibrary libskparagraph.a libskshaper.a libskunicode_core.a)
  if(EXISTS "${SKIA_BUILD_DIR}/${textLibrary}")
    list(APPEND OPENSHOW_SKIA_TEXT_LIBRARIES "${SKIA_BUILD_DIR}/${textLibrary}")
  endif()
endforeach()

target_link_libraries(openshow_skia INTERFACE
  ${OPENSHOW_SKIA_TEXT_LIBRARIES}
  "${OPENSHOW_SKIA_LIBRARY}"
  Vulkan::Vulkan
  Threads::Threads
)
