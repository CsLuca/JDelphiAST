if(NOT DEFINED TOOL OR NOT DEFINED EXPECT_EXIT OR NOT DEFINED EXPECT_TEXT)
  message(FATAL_ERROR "TOOL, EXPECT_EXIT and EXPECT_TEXT are required")
endif()

if(DEFINED EXPECT_MISSING_FILE AND DEFINED CREATE_STALE_FILE)
  file(WRITE "${EXPECT_MISSING_FILE}" "stale")
elseif(DEFINED EXPECT_MISSING_FILE)
  file(REMOVE "${EXPECT_MISSING_FILE}")
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

if(DEFINED EXPECT_MISSING_FILE AND EXISTS "${EXPECT_MISSING_FILE}")
  message(FATAL_ERROR "Output file should not exist: ${EXPECT_MISSING_FILE}")
endif()
