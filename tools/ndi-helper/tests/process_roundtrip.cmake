# Runs the real ndi-helper process with the fake backend and a command file,
# then checks the line protocol end to end: exit status, JSON-only stdout,
# answered commands, a published shared-memory name and silent stderr.
#
# Usage: cmake -DBINARY=... -DWORK_DIR=... -P process_roundtrip.cmake

if(NOT BINARY OR NOT WORK_DIR)
  message(FATAL_ERROR "process_roundtrip.cmake needs -DBINARY and -DWORK_DIR")
endif()

set(commands "${WORK_DIR}/helper-roundtrip-commands.jsonl")
file(WRITE "${commands}"
  "{\"cmd\":\"probe\"}\n"
  "{\"cmd\":\"probe\",\"protocol\":1}\n"
  "{\"cmd\":\"list\",\"waitMs\":0}\n"
  "{\"cmd\":\"open\",\"source\":\"Fake camera 1\"}\n"
  "{\"cmd\":\"status\"}\n"
  "{\"cmd\":\"close\"}\n"
  "{\"cmd\":\"quit\"}\n"
)

execute_process(
  COMMAND "${BINARY}" --fake --input "${commands}"
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
  RESULT_VARIABLE result
  TIMEOUT 30
)

if(NOT result EQUAL 0)
  message(FATAL_ERROR "helper exited with ${result}\nstderr:\n${err}\nstdout:\n${out}")
endif()

if(NOT err STREQUAL "")
  message(FATAL_ERROR "a clean helper run must not write to stderr:\n${err}")
endif()

string(REGEX REPLACE "\n$" "" out "${out}")
string(REPLACE "\n" ";" replyLines "${out}")
list(LENGTH replyLines count)
if(NOT count EQUAL 7)
  message(FATAL_ERROR "expected 7 replies, got ${count}:\n${out}")
endif()

foreach(line ${replyLines})
  if(NOT line MATCHES "^\\{")
    message(FATAL_ERROR "stdout line is not a JSON object: ${line}")
  endif()
  if(NOT line MATCHES "\"ok\":")
    message(FATAL_ERROR "stdout line carries no 'ok': ${line}")
  endif()
endforeach()

list(GET replyLines 0 probe)
if(NOT probe MATCHES "\"helper\":\"0\\.1\\.0\"")
  message(FATAL_ERROR "probe does not name the helper version: ${probe}")
endif()
if(NOT probe MATCHES "\"ndi\":\"fake\"")
  message(FATAL_ERROR "probe does not name the fake backend: ${probe}")
endif()
if(NOT probe MATCHES "\"protocol\":1")
  message(FATAL_ERROR "probe does not name protocol 1: ${probe}")
endif()

list(GET replyLines 2 list)
if(NOT list MATCHES "Fake camera 1")
  message(FATAL_ERROR "list does not carry the fake sources: ${list}")
endif()

list(GET replyLines 3 open)
if(NOT open MATCHES "\"shm\":\"/openshow-ndi-")
  message(FATAL_ERROR "open does not publish a segment name: ${open}")
endif()
if(NOT open MATCHES "\"width\":320")
  message(FATAL_ERROR "open does not report the fake width: ${open}")
endif()
if(NOT open MATCHES "\"format\":\"BGRA\"")
  message(FATAL_ERROR "open does not report BGRA: ${open}")
endif()

list(GET replyLines 4 status)
if(NOT status MATCHES "\"open\":true")
  message(FATAL_ERROR "status does not report the open source: ${status}")
endif()

list(GET replyLines 5 close)
if(NOT close MATCHES "\"ok\":true")
  message(FATAL_ERROR "close failed: ${close}")
endif()

# EOF with no quit is also a clean exit.
set(eofCommands "${WORK_DIR}/helper-roundtrip-eof.jsonl")
file(WRITE "${eofCommands}" "{\"cmd\":\"probe\"}\n")
execute_process(
  COMMAND "${BINARY}" --fake --input "${eofCommands}"
  OUTPUT_VARIABLE eofOut
  ERROR_VARIABLE eofErr
  RESULT_VARIABLE eofResult
  TIMEOUT 30
)
if(NOT eofResult EQUAL 0)
  message(FATAL_ERROR "helper did not exit cleanly on EOF: ${eofResult}\n${eofErr}")
endif()
if(NOT eofOut MATCHES "\"ok\":true")
  message(FATAL_ERROR "helper did not answer before EOF: ${eofOut}")
endif()

message(STATUS "helper process round trip passed")
