# The sql command line tool and, with SQL_TOOL_WEB, its browser front end:
#   src/sql_tool.cpp       the command line (prompt, -c, -f, --listen)
#   src/web_server.cpp     the HTTP server behind --listen: the page, the SQL WebSocket route and the report files (library sqlweb)
#   src/reports.cpp        the report definitions on disk (one JSON file each)
#   fonts/                 the report font (Liberation Sans, SIL OFL), embedded for the designer and the PDF export
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

if(SQL_TOOL_WEB AND NOT SQL_WITH_REMOTE)
    message(WARNING "the sql tool's web interface serves the remote layer: with SQL_WITH_REMOTE=OFF it is built without --listen.")
    set(SQL_TOOL_WEB OFF)
endif()

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

    # The report features (designer, runner, report files, the embedded font) are part of the remote layer: they come
    # with SQL_WITH_REMOTE, and SQL_WITH_REPORTS=OFF leaves them, and the font, out of the artifacts.
    if(SQL_WITH_REMOTE AND SQL_WITH_REPORTS)
        target_sources(sqlweb PRIVATE src/reports.cpp)
        target_compile_definitions(sqlweb PUBLIC SQL_TOOL_REPORTS=1)
        sql_embed_fonts(sqlweb)
    endif()

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
            # The page the binary embeds speaks librpc, not the retired protocol (rebuild it with `npm run b` in ui/).
            add_test(NAME ui_embedded_page_current
                     COMMAND ${CMAKE_COMMAND} -DPAGE=${CMAKE_CURRENT_SOURCE_DIR}/public/index.html
                             -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckEmbeddedPage.cmake)
            # The real page in a real browser, when one is installed (CHROME_BIN, or a Playwright download).
            file(GLOB SQL_CHROME_HINTS "$ENV{HOME}/.cache/ms-playwright/chromium_headless_shell-*/chrome-headless-shell-linux*")
            find_program(SQL_CHROME NAMES chrome-headless-shell chromium chromium-browser google-chrome
                         HINTS $ENV{CHROME_BIN} ${SQL_CHROME_HINTS})
            if(SQL_CHROME)
                add_test(NAME web_ui_browser COMMAND ${SQL_NODE} scripts/browser-e2e.mjs $<TARGET_FILE:sql_cli> ${SQL_CHROME}
                         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/ui)
                set_tests_properties(web_ui_browser PROPERTIES TIMEOUT 120)
            endif()
            # The report engine (layout, PDF, fonts, expressions, CSV) needs no browser and no server.
            add_test(NAME ui_reports_unit COMMAND ${SQL_NODE} scripts/unit.mjs WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/ui)
            set_tests_properties(ui_reports_unit PROPERTIES TIMEOUT 120)
            if(SQL_CHROME AND SQL_WITH_REMOTE AND SQL_WITH_REPORTS)
                # Running, viewing, exporting and designing reports in the page, and the demo's reports.
                add_test(NAME web_ui_reports COMMAND ${SQL_NODE} scripts/reports-e2e.mjs $<TARGET_FILE:sql_cli> ${SQL_CHROME}
                         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/ui)
                set_tests_properties(web_ui_reports PROPERTIES TIMEOUT 180)
                add_test(NAME web_ui_demo COMMAND ${SQL_NODE} scripts/demo-e2e.mjs $<TARGET_FILE:sql_cli> ${SQL_CHROME}
                         ${CMAKE_CURRENT_SOURCE_DIR}/../../demo WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/ui)
                set_tests_properties(web_ui_demo PROPERTIES TIMEOUT 180)
                # The mouse on the charts and tables, and the designer's click, drag and undo.
                add_test(NAME web_ui_interactive COMMAND ${SQL_NODE} scripts/interactive-e2e.mjs $<TARGET_FILE:sql_cli> ${SQL_CHROME}
                         ${CMAKE_CURRENT_SOURCE_DIR}/../../demo WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/ui)
                set_tests_properties(web_ui_interactive PROPERTIES TIMEOUT 240)
            endif()
            # The TypeScript generated from remote.bufs and the vendored librpc runtime match their sources.
            add_test(NAME ui_wire_up_to_date COMMAND ${SQL_NODE} scripts/vendor-librpc.mjs $<TARGET_FILE:rpcc> --check
                     WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/ui)
        endif()
        if(SQL_WITH_BUFS)
            add_subdirectory(../bufs_golden bufs_golden)
        endif()
    endif()
endif()
