# Fails when application or test source includes or names the proprietary NDI
# SDK. The GPL application must reach NDI only through the separate
# tools/ndi-helper process; the shared frame-ring header is in shared/, not
# src/, and carries no NDI API. Usage:
#   cmake -DROOT=/path/to/repo -DSRC_DIRS="src;tests" -P CheckNoNdi.cmake

if(NOT ROOT OR NOT SRC_DIRS)
  message(FATAL_ERROR
    "CheckNoNdi.cmake needs -DROOT=<repo> -DSRC_DIRS=<dirs, relative to ROOT>")
endif()

string(REGEX REPLACE "[ \t\"]" "" srcDirs "${SRC_DIRS}")

set(violations "")
foreach(dir ${srcDirs})
  file(GLOB_RECURSE sources RELATIVE "${ROOT}"
    "${ROOT}/${dir}/*.cpp" "${ROOT}/${dir}/*.h" "${ROOT}/${dir}/*.hpp"
    "${ROOT}/${dir}/*.cc")
  foreach(source ${sources})
    # tests/fixtures/ holds this guard's own negative test data (files that
    # MUST trip it); real code lives in src/ and tests/ proper.
    if(source MATCHES "/fixtures/")
      continue()
    endif()
    file(STRINGS "${ROOT}/${source}" lines)
    set(lineno 0)
    foreach(line ${lines})
      math(EXPR lineno "${lineno} + 1")
      if(line MATCHES "Processing\\.NDI|NDIlib_[A-Za-z]")
        string(APPEND violations "\n  ${source}:${lineno}: ${line}")
      endif()
    endforeach()
  endforeach()
endforeach()

if(violations)
  message(FATAL_ERROR
    "The GPL application must not include or name the proprietary NDI SDK "
    "(see docs/ndi-camera-input.md; NDI lives behind tools/ndi-helper):"
    "${violations}")
endif()
message(STATUS "no NDI SDK references in application or test source")
