if(NOT DEFINED TOOL OR NOT DEFINED INDEX)
  message(FATAL_ERROR "TOOL and INDEX are required")
endif()

set(sidecar "${INDEX}.qidx")
file(REMOVE "${sidecar}")

execute_process(COMMAND "${TOOL}" build-query-index --index "${INDEX}" --format json
  RESULT_VARIABLE first_result OUTPUT_VARIABLE first_output ERROR_VARIABLE first_error)
if(NOT first_result EQUAL 0 OR NOT first_output MATCHES "cache_status[^\n]*rebuilt")
  message(FATAL_ERROR "Expected sidecar rebuild. Output: ${first_output} Error: ${first_error}")
endif()

execute_process(COMMAND "${TOOL}" build-query-index --index "${INDEX}" --format json
  RESULT_VARIABLE second_result OUTPUT_VARIABLE second_output ERROR_VARIABLE second_error)
if(NOT second_result EQUAL 0 OR NOT second_output MATCHES "cache_status[^\n]*hit")
  message(FATAL_ERROR "Expected sidecar cache hit. Output: ${second_output} Error: ${second_error}")
endif()

if(NOT EXISTS "${sidecar}")
  message(FATAL_ERROR "Expected query sidecar: ${sidecar}")
endif()
