if(NOT DEFINED SLT_RUNNER)
    message(FATAL_ERROR "SLT_RUNNER is required")
endif()
if(NOT DEFINED SMOKE_FILE)
    message(FATAL_ERROR "SMOKE_FILE is required")
endif()

execute_process(
    COMMAND "${SLT_RUNNER}" --engine sqlite "${SMOKE_FILE}"
    RESULT_VARIABLE sqlite_result)
if(NOT sqlite_result EQUAL 0)
    message(FATAL_ERROR "SQLite smoke run failed")
endif()

execute_process(
    COMMAND "${SLT_RUNNER}" --engine libsql "${SMOKE_FILE}"
    RESULT_VARIABLE libsql_result)
if(NOT libsql_result EQUAL 0)
    message(FATAL_ERROR "libsql smoke run failed")
endif()
