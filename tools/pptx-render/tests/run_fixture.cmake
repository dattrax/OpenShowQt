# PX.1 acceptance: run the built sidecar on the committed fixture and prove
# the product exists at the documented size. Run with:
#   cmake -DBIN=... -DFIXTURE=... -DOUT_DIR=... -P run_fixture.cmake
file(REMOVE_RECURSE "${OUT_DIR}")
execute_process(
  COMMAND "${BIN}" --input "${FIXTURE}" --out-dir "${OUT_DIR}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "pptx-render failed (${result}): ${stderr}")
endif()
foreach(name slide-01.png slide-02.png manifest.json)
  if(NOT EXISTS "${OUT_DIR}/${name}")
    message(FATAL_ERROR "missing ${OUT_DIR}/${name}; output: ${stdout}")
  endif()
endforeach()
if(EXISTS "${OUT_DIR}/slide-03.png")
  message(FATAL_ERROR "hidden page 3 was rendered")
endif()
file(READ "${OUT_DIR}/manifest.json" manifest)
foreach(needle "\"hiddenPages\"" "\"page\": 1" "\"page\": 2" "\"width\": 1920" "\"height\": 1080")
  string(FIND "${manifest}" "${needle}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "manifest missing ${needle}:\n${manifest}")
  endif()
endforeach()
message(STATUS "pptx-render fixture: ${stdout}")
