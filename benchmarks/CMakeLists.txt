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
