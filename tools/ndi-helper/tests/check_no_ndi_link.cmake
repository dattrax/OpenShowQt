# Fails when BINARY links the proprietary NDI runtime. The helper loads NDI
# with dlopen only, and the fake-only build must not need it at all.
#
# Usage: cmake -DBINARY=/path/to/ndi-helper -P check_no_ndi_link.cmake

if(NOT BINARY)
  message(FATAL_ERROR "check_no_ndi_link.cmake needs -DBINARY=/path/to/helper")
endif()

if(WIN32)
  # ldd does not exist on Windows; the fake-only build has no import library to
  # inspect here, and the link line is the real check.
  message(STATUS "check_no_ndi_link: skipped on Windows")
  return()
endif()

execute_process(
  COMMAND ldd "${BINARY}"
  OUTPUT_VARIABLE linked
  ERROR_VARIABLE linkError
  RESULT_VARIABLE linkResult
)

if(NOT linkResult EQUAL 0)
  message(FATAL_ERROR "ldd ${BINARY} failed (${linkResult}): ${linkError}")
endif()

if(linked MATCHES "libndi")
  message(FATAL_ERROR
    "the helper must load NDI with dlopen, never link it:\n${linked}")
endif()

message(STATUS "helper does not link the NDI runtime")
