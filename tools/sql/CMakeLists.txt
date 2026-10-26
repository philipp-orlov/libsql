# The sql command line tool and, with SQL_TOOL_WEB, its browser front end:
#   src/sql_tool.cpp       the command line (prompt, -c, -f, --listen)
#   src/web_server.cpp     the HTTP server behind --listen: the page and the SQL WebSocket route (library sqlweb)
#   ui/                    the Angular front end; `npm run b` there writes public/index.html
#   public/index.html      the built front end, linked into the binary (cmake/EmbedIndexHtml.cmake)
#   tests/                 tests of the web part, run by ctest with SQL_BUILD_TESTS

option(SQL_TOOL_WEB "Build the sql tool's web interface (--listen; needs libhttp)" ON)
set(SQL_HTTP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../libhttp"
    CACHE PATH "Source directory of libhttp, when it is not already part of the build")

add_executable(sql_cli src/sql_tool.cpp)
target_link_libraries(sql_cli PRIVATE sql)
set_target_properties(sql_cli PROPERTIES OUTPUT_NAME sql)
install(TARGETS sql_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(SQL_TOOL_WEB AND NOT TARGET http)
    if(NOT EXISTS "${SQL_HTTP_DIR}/CMakeLists.txt")
        message(WARNING "libhttp not found at ${SQL_HTTP_DIR}: the sql tool is built without --listen. "
                        "Point -DSQL_HTTP_DIR at its source tree, or set -DSQL_TOOL_WEB=OFF.")
        set(SQL_TOOL_WEB OFF)
    else()
        set(HTTP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(HTTP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(HTTP_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
        add_subdirectory("${SQL_HTTP_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/libhttp")
    endif()
endif()

if(SQL_TOOL_WEB)
    include(cmake/EmbedIndexHtml.cmake)

    add_library(sqlweb STATIC src/web_server.cpp)
    target_include_directories(sqlweb PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(sqlweb PUBLIC sql http)
    if(MSVC)
        target_compile_options(sqlweb PRIVATE /W4 /permissive-)
    else()
        target_compile_options(sqlweb PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    sql_embed_index_html(sqlweb)

    target_link_libraries(sql_cli PRIVATE sqlweb)
    target_compile_definitions(sql_cli PRIVATE SQL_TOOL_WEB=1)

    if(SQL_BUILD_TESTS AND NOT WIN32)
        add_subdirectory(tests)
        # The front end's data layer against the real binary, when node and the UI's packages are there.
        find_program(SQL_NODE node)
        if(SQL_NODE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ui/node_modules/esbuild")
            add_test(NAME web_ui_e2e COMMAND ${SQL_NODE} scripts/e2e.mjs $<TARGET_FILE:sql_cli>
                     WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/ui)
            set_tests_properties(web_ui_e2e PROPERTIES TIMEOUT 120)
        endif()
        if(SQL_WITH_BUFS)
            add_subdirectory(../bufs_golden bufs_golden)
        endif()
    endif()
endif()
