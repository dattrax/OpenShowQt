# Fails when TARGET links a Qt library, or, with CHECK=linked, when it does not.
# Usage: cmake -DTARGET=/path/to/binary [-DCHECK=linked] -P CheckNoQt.cmake

if(NOT TARGET)
  message(FATAL_ERROR "CheckNoQt.cmake needs -DTARGET=/path/to/binary")
endif()

execute_process(
  COMMAND ldd ${TARGET}
  OUTPUT_VARIABLE lddOutput
  ERROR_VARIABLE lddError
  RESULT_VARIABLE lddResult
)

if(NOT lddResult EQUAL 0)
  message(FATAL_ERROR "ldd ${TARGET} failed (${lddResult}): ${lddError}")
endif()

string(REGEX MATCH "libQt6[A-Za-z0-9]*" qtLibrary "${lddOutput}")

if(CHECK STREQUAL "linked")
  if(NOT qtLibrary)
    message(FATAL_ERROR "${TARGET} should link Qt but does not")
  endif()
  message(STATUS "${TARGET} links ${qtLibrary}")
else()
  if(qtLibrary)
    message(FATAL_ERROR "${TARGET} must stay Qt-free but links ${qtLibrary}")
  endif()
  message(STATUS "${TARGET} is Qt-free")
endif()
