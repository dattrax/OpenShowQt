# Task 6.16b: the shell has to LOAD.
#
# QML is not compiled, so the class of mistake this guards has no other check:
# an item property that was written as a C++ accessor pair and never declared
# with Q_PROPERTY, a `root.` where the id was another object's, a function
# handed to Qt.callLater as an undefined value. Every one of those compiles,
# installs, and shows the operator nothing — and the first version of the
# slide list's rows did all three at once, found only because someone ran the
# binary.
#
# So: start the real executable with the offscreen platform plugin and no real
# worker (OPENSHOW_WORKER_PROGRAM=/bin/false: the shell is under test, not the
# compositor, and a fullscreen window on the operator's display has no part in
# it), let it live for a few seconds, and ask two questions.
#
#   - did it STAY up? A shell whose root component fails to load returns -1,
#     so the run timing out is the pass and an early exit is the failure.
#   - did anything it printed say so? Load errors are the ones listed below.
#     Five notices are known and predate this check — three Shortcut bindings
#     that name one of several sequences, two Flow children using anchors —
#     and they are named here rather than whitelisted by line number, so that
#     a NEW one of the same kind still reads as one line of output to explain.

if(NOT DEFINED TARGET)
  message(FATAL_ERROR "CheckUiLoads.cmake wants -DTARGET=<path to openshow-ui>")
endif()
if(NOT EXISTS ${TARGET})
  message(FATAL_ERROR "no executable at ${TARGET}")
endif()

set(ENV{QT_QPA_PLATFORM} "offscreen")
set(ENV{QT_FORCE_STDERR_LOGGING} "1")
set(ENV{OPENSHOW_WORKER_PROGRAM} "/bin/false")
if(DEFINED SMOKE_SECONDS)
  set(runSeconds ${SMOKE_SECONDS})
else()
  set(runSeconds 8)
endif()

set(output "")
if(DEFINED FAKE_OUTPUT)
  # This script's own teeth, for the negative test: read the output a broken
  # shell produced on some other machine instead of running one here.
  file(READ "${FAKE_OUTPUT}" output)
  set(status "Process terminated due to timeout")
else()
  execute_process(
    COMMAND ${TARGET}
    TIMEOUT ${runSeconds}
    RESULT_VARIABLE status
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE)
  set(output "${out}${err}")
endif()

# An early exit is the load failing (a root component that does not load makes
# main return -1); a timeout means the shell was still standing when this check
# stopped it, which is the wanted answer. Anything else — could not start,
# crashed — is a failure too, so the pass is stated narrowly rather than as
# "not a number".
if(status MATCHES "^[0-9]+$")
  message(FATAL_ERROR
          "openshow-ui exited with code ${status} instead of staying up - the QML "
          "module did not load. Output:\n${output}")
endif()
if(NOT status MATCHES "timeout")
  message(FATAL_ERROR
          "openshow-ui could not be run for this check ('${status}'). Output:\n${output}")
endif()

set(forbidden
  "failed to load"
  ": Error:"
  "Cannot assign to non-existent property"
  "is not a function"
  "is not a type"
  "is undefined")

set(complaints "")
string(REPLACE "\n" ";" lines "${output}")
foreach(line IN LISTS lines)
  foreach(pattern IN LISTS forbidden)
    if(line MATCHES "${pattern}")
      string(APPEND complaints "  ${line}\n")
      break()
    endif()
  endforeach()
endforeach()

if(complaints)
  message(FATAL_ERROR
          "openshow-ui loaded with complaints:\n${complaints}Full output:\n${output}")
endif()

message(STATUS "openshow-ui stayed up ${runSeconds}s offscreen with no load errors "
               "(worker suppressed; pre-existing notices ignored)")
