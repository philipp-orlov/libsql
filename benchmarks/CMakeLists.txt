# Benchmarks are executables, not tests: they print a table and optionally a
# JSON/CSV file, and are meant to be run pinned to a core in Release.
add_executable(bench_sql bench_sql.cpp)
target_link_libraries(bench_sql PRIVATE sql)
target_include_directories(bench_sql PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(MSVC)
    target_compile_options(bench_sql PRIVATE /W4 /permissive- /Zc:__cplusplus)
    target_compile_definitions(bench_sql PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(bench_sql PRIVATE psapi)
else()
    target_compile_options(bench_sql PRIVATE -Wall -Wextra)
endif()

# The same workloads against the SQLite amalgamation the conformance harness
# fetches (configure with -DSQL_BUILD_CONFORMANCE=ON as well).
if(TARGET sqlite3_oracle)
    add_executable(bench_vs_sqlite bench_vs_sqlite.cpp)
    target_link_libraries(bench_vs_sqlite PRIVATE sql sqlite3_oracle)
    target_include_directories(bench_vs_sqlite PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    if(NOT MSVC)
        target_compile_options(bench_vs_sqlite PRIVATE -Wall -Wextra)
        target_link_libraries(bench_vs_sqlite PRIVATE ${CMAKE_DL_LIBS})
    endif()
endif()

# Binary results against JSON (needs SQL_WITH_BUFS). zlib, when found, adds gzip sizes.
if(SQL_WITH_BUFS)
    add_executable(bench_bufs bench_bufs.cpp)
    target_link_libraries(bench_bufs PRIVATE sql)
    target_include_directories(bench_bufs PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    find_package(ZLIB QUIET)
    if(ZLIB_FOUND)
        target_link_libraries(bench_bufs PRIVATE ZLIB::ZLIB)
    endif()
    if(NOT MSVC)
        target_compile_options(bench_bufs PRIVATE -Wall -Wextra)
    endif()
endif()
