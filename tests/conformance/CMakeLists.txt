enable_language(C)

set(SQLITE_AMALGAMATION_VERSION 3450100)
set(SQLITE_AMALGAMATION_URL
    "https://www.sqlite.org/2024/sqlite-amalgamation-${SQLITE_AMALGAMATION_VERSION}.zip")
set(SQLITE_AMALGAMATION_HASH
    "SHA256=5592243caf28b2cdef41e6ab58d25d653dfc53deded8450eb66072c929f030c4")
set(SQLITE_DOWNLOAD_DIR "${CMAKE_CURRENT_BINARY_DIR}/sqlite-download")
set(SQLITE_ZIP "${SQLITE_DOWNLOAD_DIR}/sqlite-amalgamation-${SQLITE_AMALGAMATION_VERSION}.zip")
set(SQLITE_SOURCE_DIR
    "${SQLITE_DOWNLOAD_DIR}/sqlite-amalgamation-${SQLITE_AMALGAMATION_VERSION}")

file(MAKE_DIRECTORY "${SQLITE_DOWNLOAD_DIR}")
file(DOWNLOAD "${SQLITE_AMALGAMATION_URL}" "${SQLITE_ZIP}"
     EXPECTED_HASH "${SQLITE_AMALGAMATION_HASH}"
     SHOW_PROGRESS)

if(NOT EXISTS "${SQLITE_SOURCE_DIR}/sqlite3.c")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E tar xzf "${SQLITE_ZIP}"
        WORKING_DIRECTORY "${SQLITE_DOWNLOAD_DIR}"
        COMMAND_ERROR_IS_FATAL ANY)
endif()

add_library(sqlite3_oracle STATIC "${SQLITE_SOURCE_DIR}/sqlite3.c")
target_compile_definitions(sqlite3_oracle
    PRIVATE
        SQLITE_ENABLE_MATH_FUNCTIONS
        SQLITE_DQS=0
        SQLITE_THREADSAFE=0
        SQLITE_OMIT_LOAD_EXTENSION
        SQLITE_DEFAULT_FOREIGN_KEYS=0
        SQLITE_SOUNDEX)
target_include_directories(sqlite3_oracle PUBLIC "${SQLITE_SOURCE_DIR}")

add_library(conformance_common STATIC
    conformance_util.cpp
)
target_link_libraries(conformance_common PUBLIC sql sqlite3_oracle)
target_include_directories(conformance_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(conformance_common PUBLIC cxx_std_17)

add_executable(slt_runner slt_runner.cpp)
target_link_libraries(slt_runner PRIVATE conformance_common)

add_executable(difftest
    difftest.cpp
    sqlgen.cpp
)
target_link_libraries(difftest PRIVATE conformance_common)

if(MSVC)
    target_compile_options(conformance_common PRIVATE /W4 /permissive- /Zc:__cplusplus)
    target_compile_options(slt_runner PRIVATE /W4 /permissive- /Zc:__cplusplus)
    target_compile_options(difftest PRIVATE /W4 /permissive- /Zc:__cplusplus)
else()
    target_compile_options(conformance_common PRIVATE -Wall -Wextra)
    target_compile_options(slt_runner PRIVATE -Wall -Wextra)
    target_compile_options(difftest PRIVATE -Wall -Wextra)
endif()

add_test(NAME conformance_smoke
    COMMAND "${CMAKE_COMMAND}"
        -DSLT_RUNNER=$<TARGET_FILE:slt_runner>
        -DSMOKE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/smoke/basic.test
        -P ${CMAKE_CURRENT_SOURCE_DIR}/run-smoke.cmake)
set_tests_properties(conformance_smoke PROPERTIES
    LABELS conformance
    TIMEOUT 120)

# Hand-written scripts for what the sqllogictest corpus and the fuzzer do not
# reach (triggers, foreign keys, views, ALTER TABLE, savepoints, windows,
# functions): every statement must match SQLite exactly.
file(GLOB SQL_CONFORMANCE_SCRIPTS "${CMAKE_CURRENT_SOURCE_DIR}/scripts/*.sql")
add_test(NAME conformance_scripts COMMAND difftest ${SQL_CONFORMANCE_SCRIPTS})
set_tests_properties(conformance_scripts PROPERTIES LABELS conformance TIMEOUT 300)

# A short differential fuzz run; the 24-hour run is `difftest --fuzz` by hand.
add_test(NAME conformance_fuzz COMMAND difftest --fuzz --seed 7 --count 500)
set_tests_properties(conformance_fuzz PROPERTIES LABELS conformance TIMEOUT 600)
