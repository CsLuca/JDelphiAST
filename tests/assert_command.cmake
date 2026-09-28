if(NOT DEFINED TOOL OR NOT DEFINED EXPECT_EXIT OR NOT DEFINED EXPECT_TEXT)
  message(FATAL_ERROR "TOOL, EXPECT_EXIT and EXPECT_TEXT are required")
endif()

execute_process(
  COMMAND "${TOOL}" ${ARGS}
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error)

if(NOT result EQUAL EXPECT_EXIT)
  message(FATAL_ERROR "Expected exit ${EXPECT_EXIT}, got ${result}. Output: ${output} Error: ${error}")
endif()

if(NOT output MATCHES "${EXPECT_TEXT}")
  message(FATAL_ERROR "Output did not match ${EXPECT_TEXT}: ${output}")
endif()
