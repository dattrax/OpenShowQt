# Defines openshow::skia from a Skia tree built with GN/ninja.
#
# SKIA_ROOT:      Skia source tree, from -DSKIA_ROOT, $SKIA_ROOT, the
#                 third_party/skia submodule, or (legacy) ../skia
# SKIA_BUILD_DIR: GN output directory, from -DSKIA_BUILD_DIR, $SKIA_BUILD_DIR,
#                 or ${SKIA_ROOT}/out/Release
#
# A submodule checkout without libraries is built automatically by
# scripts/build-skia.sh (pinned revision, validated GN arguments, pinned
# DEPS); -DOPENSHOW_SKIA_AUTO_BUILD=OFF turns that off, and a prebuilt tree
# supplied through SKIA_ROOT is always used as it stands.
#
# The application does not compile shaders. Skia's own SkSL to SPIR-V code
# generator is part of libskia, so no glslc, glslang or shaderc is required.

# This project is pinned to the Skia revision it was built and validated
# against (task 6.2a): a silently mismatched libskia is the classic
# mystery-crash generator, and the worker's VulkanMemoryAdapter implements
# exactly this revision's public allocator contract. Override with
# -DOPENSHOW_SKIA_REVISION=any only after validating another revision.
# Keep in step with SKIA_REVISION in scripts/build-skia.sh.
set(OPENSHOW_SKIA_REVISION "a94df1cdabb03d3ead2486413bc2999b500aa4f8"
  CACHE STRING "Skia revision SKIA_ROOT must be at ('any' to skip the check)")

if(NOT SKIA_ROOT AND DEFINED ENV{SKIA_ROOT})
  set(SKIA_ROOT "$ENV{SKIA_ROOT}")
endif()
if(NOT SKIA_ROOT AND EXISTS "${CMAKE_SOURCE_DIR}/third_party/skia/DEPS")
  set(SKIA_ROOT "${CMAKE_SOURCE_DIR}/third_party/skia")
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

option(OPENSHOW_SKIA_AUTO_BUILD
  "Build the third_party/skia submodule with scripts/build-skia.sh when it has no libraries yet"
  ON)

set(OPENSHOW_SKIA_LIBRARY "")
foreach(candidate libskia.a libskia.so skia.lib)
  if(EXISTS "${SKIA_BUILD_DIR}/${candidate}")
    set(OPENSHOW_SKIA_LIBRARY "${SKIA_BUILD_DIR}/${candidate}")
    break()
  endif()
endforeach()

if(NOT OPENSHOW_SKIA_LIBRARY
   AND OPENSHOW_SKIA_AUTO_BUILD
   AND SKIA_ROOT STREQUAL "${CMAKE_SOURCE_DIR}/third_party/skia"
   AND EXISTS "${CMAKE_SOURCE_DIR}/scripts/build-skia.sh")
  # Task 6.2c: one-time Skia build straight out of the submodule (dependency
  # sync plus compilation takes a while; later configures find the libraries
  # and skip this). Output streams into the configure log.
  message(STATUS
    "No libskia in ${SKIA_BUILD_DIR}; building the pinned Skia submodule "
    "with scripts/build-skia.sh (one-time, this takes a while)")
  execute_process(
    COMMAND "${CMAKE_SOURCE_DIR}/scripts/build-skia.sh" "${SKIA_ROOT}" "${SKIA_BUILD_DIR}"
    RESULT_VARIABLE openshowSkiaBuildResult
  )
  if(NOT openshowSkiaBuildResult EQUAL 0)
    message(FATAL_ERROR
      "scripts/build-skia.sh failed (${openshowSkiaBuildResult}). Its output "
      "is above; rerun it by hand for an interactive look, or point "
      "-DSKIA_ROOT at a prebuilt tree.")
  endif()
  foreach(candidate libskia.a libskia.so skia.lib)
    if(EXISTS "${SKIA_BUILD_DIR}/${candidate}")
      set(OPENSHOW_SKIA_LIBRARY "${SKIA_BUILD_DIR}/${candidate}")
      break()
    endif()
  endforeach()
endif()

if(NOT OPENSHOW_SKIA_LIBRARY)
  message(FATAL_ERROR
    "No libskia found in ${SKIA_BUILD_DIR}.\n"
    "  git submodule update --init --recursive third_party/skia\n"
    "  scripts/build-skia.sh\n"
    "or point -DSKIA_ROOT at a prebuilt Skia tree at revision "
    "${OPENSHOW_SKIA_REVISION} built with Vulkan, no GL and skia_use_system_icu=true.")
endif()

add_library(openshow_skia INTERFACE IMPORTED GLOBAL)
add_library(openshow::skia ALIAS openshow_skia)

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
# (include/private, src) are deliberately NOT exported, and
# tests/CheckNoSkiaPrivate.cmake enforces the rule from the source side;
# nothing includes them since task 6.2b.
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

# Text layout: SkParagraph with its shaping and the public ICU-backed
# Unicode module (task 6.2b). This Skia is built with
# skia_use_system_icu=true, so libskunicode_icu binds the SYSTEM ICU's
# versioned symbols (ubrk_*, ubidi_*, ...): the ICU found here must be the
# major version Skia was compiled against — a mismatch shows up as a link
# error naming those symbols.
find_package(ICU COMPONENTS uc i18n REQUIRED)
set(OPENSHOW_SKIA_TEXT_LIBRARIES "")
foreach(textLibrary libskparagraph.a libskshaper.a libskunicode_core.a libskunicode_icu.a)
  if(EXISTS "${SKIA_BUILD_DIR}/${textLibrary}")
    list(APPEND OPENSHOW_SKIA_TEXT_LIBRARIES "${SKIA_BUILD_DIR}/${textLibrary}")
  endif()
endforeach()
if(NOT EXISTS "${SKIA_BUILD_DIR}/libskunicode_icu.a")
  message(FATAL_ERROR
    "${SKIA_BUILD_DIR} has no libskunicode_icu.a, but the worker's Unicode "
    "backend is the ICU-backed module (task 6.2b). Rebuild this Skia tree "
    "with skia_use_system_icu=true — scripts/build-skia.sh uses the right "
    "arguments.")
endif()

target_link_libraries(openshow_skia INTERFACE
  ${OPENSHOW_SKIA_TEXT_LIBRARIES}
  "${OPENSHOW_SKIA_LIBRARY}"
  ICU::i18n
  ICU::uc
  Vulkan::Vulkan
  Threads::Threads
)
