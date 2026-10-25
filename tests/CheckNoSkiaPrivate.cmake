# Fails when any .cpp/.h under SRC_DIRS includes a Skia private or src tree
# header (task 6.2a, review R9): "include/private/...", "src/..." or
# "modules/<m>/src/...". Public module headers (modules/<m>/include/...)
# are allowed. Exemptions are repository-relative file names, each one owed
# by a task. Usage:
#   cmake -DROOT=/path/to/repo -DSRC_DIRS="src;tests" -P CheckNoSkiaPrivate.cmake

if(NOT ROOT OR NOT SRC_DIRS)
  message(FATAL_ERROR
    "CheckNoSkiaPrivate.cmake needs -DROOT=<repo> -DSRC_DIRS=<dirs, relative to ROOT>")
endif()

# SceneText.cpp still needs SkTArray/SkUTF/the hardcoded SkUnicode backend
# (plus an ICU symbol shim); task 6.2b moves it to public APIs and this
# list empties. Nothing may be added here without a PLAN.md entry.
set(EXEMPT_FILES
  "src/worker/render/SceneText.cpp"
)

string(REGEX REPLACE "[ \t\"]" "" srcDirs "${SRC_DIRS}")

set(violations "")
foreach(dir ${srcDirs})
  file(GLOB_RECURSE sources RELATIVE "${ROOT}"
    "${ROOT}/${dir}/*.cpp" "${ROOT}/${dir}/*.h" "${ROOT}/${dir}/*.hpp"
    "${ROOT}/${dir}/*.cc")
  foreach(source ${sources})
    if(source IN_LIST EXEMPT_FILES)
      continue()
    endif()
    # tests/fixtures/ holds this guard's own negative test data (files
    # that MUST trip it); real code lives in src/ and tests/ proper.
    if(source MATCHES "/fixtures/")
      continue()
    endif()
    file(STRINGS "${ROOT}/${source}" lines)
    set(lineno 0)
    foreach(line ${lines})
      math(EXPR lineno "${lineno} + 1")
      if(line MATCHES "^#[ \t]*include[ \t]*\"(src/|include/private/|modules/[^/\"]+/src/)")
        string(APPEND violations "\n  ${source}:${lineno}: ${line}")
      endif()
    endforeach()
  endforeach()
endforeach()

if(violations)
  message(FATAL_ERROR
    "Skia private/src headers must not be included from application or "
    "test code (public Skia APIs only — see PLAN.md task 6.2a, review R9):"
    "${violations}")
endif()
message(STATUS "no Skia private/src includes outside the exemption list")
