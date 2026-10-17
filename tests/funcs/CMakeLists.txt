# Oracle replay tests for the function modules: every case was recorded from
# SQLite 3.45.1 by the gen_*_cases.py scripts beside them (regenerate with
# Python's sqlite3 when the pinned version moves).
foreach(name json date printf)
    add_executable(test_${name}_funcs test_${name}_funcs.cpp)
    target_link_libraries(test_${name}_funcs PRIVATE sql)
    target_include_directories(test_${name}_funcs PRIVATE ${PROJECT_SOURCE_DIR}/src)
    add_test(NAME ${name}_funcs
             COMMAND test_${name}_funcs ${CMAKE_CURRENT_SOURCE_DIR}/${name}_cases.tsv)
    set_tests_properties(${name}_funcs PROPERTIES TIMEOUT 300)
endforeach()
# The date cases include localtime conversions recorded in this zone.
set_tests_properties(date_funcs PROPERTIES ENVIRONMENT "TZ=America/New_York")
