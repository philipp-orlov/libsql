# sql_embed_index_html(<target>): embeds public/index.html, gzipped, into <target> via objcopy and
# defines SQL_EMBEDDED_INDEX_HTML for it, so web_server.cpp serves the front end from the binary.
#
# The default deployment is then the single executable; --public still serves from disk instead. Needs
# GNU binutils' objcopy/objdump and zlib; without them, or without public/index.html (npm run b in ui/
# makes it), the binary is built without the embedded page. The same method isvc uses, see
# isvc/cmake/EmbedIndexHtml.cmake.

set(SQL_PUBLIC_DIR "${CMAKE_CURRENT_LIST_DIR}/../public")
set(SQL_FONTS_DIR "${CMAKE_CURRENT_LIST_DIR}/../fonts")  # (a function sees the caller's CMAKE_CURRENT_LIST_DIR, so it is set here)

function(sql_embed_index_html target)
    set(index_html "${SQL_PUBLIC_DIR}/index.html")
    if(NOT (CMAKE_OBJCOPY AND CMAKE_OBJDUMP AND EXISTS "${index_html}"))
        message(STATUS "${target}: no embedded index.html (needs objcopy, objdump and public/index.html)")
        return()
    endif()
    find_program(SQL_GZIP gzip)
    find_package(ZLIB)
    if(NOT (SQL_GZIP AND ZLIB_FOUND))
        message(WARNING "${target}: gzip or zlib not found -- index.html will not be embedded; --public remains required")
        return()
    endif()
    set(work "${CMAKE_CURRENT_BINARY_DIR}/${target}_embed")
    file(MAKE_DIRECTORY "${work}")

    # objcopy's `-I binary` input has no format to take -O/-B from, so they are read from a throwaway
    # object this toolchain just built rather than hardcoding a triple that fits one platform.
    set(probe_src "${work}/embed_probe.cpp")
    set(probe_obj "${work}/embed_probe.o")
    file(WRITE "${probe_src}" "int sqlEmbedProbe;\n")
    execute_process(
        COMMAND ${CMAKE_CXX_COMPILER} -c "${probe_src}" -o "${probe_obj}"
        RESULT_VARIABLE probe_result
    )
    set(output_target "")
    set(arch "")
    if(probe_result EQUAL 0)
        execute_process(COMMAND ${CMAKE_OBJDUMP} -f "${probe_obj}" OUTPUT_VARIABLE probe_info)
        string(REGEX MATCH "file format ([^ \n]+)" _ "${probe_info}")
        set(output_target "${CMAKE_MATCH_1}")
        string(REGEX MATCH "architecture: ([^,]+)," _ "${probe_info}")
        set(arch "${CMAKE_MATCH_1}")
    endif()
    if(NOT (output_target AND arch))
        message(WARNING "${target}: could not determine objcopy's output target/architecture "
                        "-- index.html will not be embedded; --public remains required")
        return()
    endif()

    set(embedded "${work}/embedded_index_html.o")
    add_custom_command(
        OUTPUT "${embedded}"
        # Run in a directory holding only index.html.gz, so that the _binary_index_html_gz_{start,end}
        # names objcopy derives from the file name do not depend on this repository's layout.
        COMMAND ${CMAKE_COMMAND} -E copy "${index_html}" "${work}/index.html"
        COMMAND ${SQL_GZIP} -9 -n -f "${work}/index.html"
        COMMAND ${CMAKE_OBJCOPY} -I binary -O ${output_target} -B ${arch} index.html.gz "${embedded}"
        # Without this section the linker assumes the object needs an executable stack.
        COMMAND ${CMAKE_OBJCOPY} --add-section .note.GNU-stack=/dev/null
                --set-section-flags .note.GNU-stack=noload,readonly "${embedded}"
        WORKING_DIRECTORY "${work}"
        DEPENDS "${index_html}"
        COMMENT "Embedding gzipped public/index.html into ${target} via objcopy"
        VERBATIM
    )
    set_source_files_properties("${embedded}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
    target_sources(${target} PRIVATE "${embedded}")
    target_compile_definitions(${target} PRIVATE SQL_EMBEDDED_INDEX_HTML=1)
    target_link_libraries(${target} PRIVATE ZLIB::ZLIB)
endfunction()

# sql_embed_fonts(<target>): embeds fonts/LiberationSans-{Regular,Bold}.ttf, gzipped, the way the page is, and defines
# SQL_EMBEDDED_FONTS for it: web_server.cpp serves them at /fonts/ for the report designer and the PDF export. Without
# them the report features still run, but with the browser's fonts and no PDF (the page says so).
function(sql_embed_fonts target)
    set(fonts_dir "${SQL_FONTS_DIR}")
    if(NOT (CMAKE_OBJCOPY AND CMAKE_OBJDUMP AND EXISTS "${fonts_dir}/LiberationSans-Regular.ttf"))
        message(STATUS "${target}: no embedded report fonts (needs objcopy, objdump and fonts/)")
        return()
    endif()
    find_program(SQL_GZIP gzip)
    if(NOT SQL_GZIP)
        return()
    endif()
    set(work "${CMAKE_CURRENT_BINARY_DIR}/${target}_fonts")
    file(MAKE_DIRECTORY "${work}")
    set(probe_src "${work}/embed_probe.cpp")
    set(probe_obj "${work}/embed_probe.o")
    file(WRITE "${probe_src}" "int sqlEmbedProbe;\n")
    execute_process(COMMAND ${CMAKE_CXX_COMPILER} -c "${probe_src}" -o "${probe_obj}" RESULT_VARIABLE probe_result)
    execute_process(COMMAND ${CMAKE_OBJDUMP} -f "${probe_obj}" OUTPUT_VARIABLE probe_info)
    string(REGEX MATCH "file format ([^ \n]+)" _ "${probe_info}")
    set(output_target "${CMAKE_MATCH_1}")
    string(REGEX MATCH "architecture: ([^,]+)," _ "${probe_info}")
    set(arch "${CMAKE_MATCH_1}")
    if(NOT (probe_result EQUAL 0 AND output_target AND arch))
        return()
    endif()
    set(objects "")
    foreach(face Regular Bold)
        set(font "${fonts_dir}/LiberationSans-${face}.ttf")
        set(object "${work}/LiberationSans-${face}.o")
        add_custom_command(
            OUTPUT "${object}"
            COMMAND ${CMAKE_COMMAND} -E copy "${font}" "${work}/LiberationSans-${face}.ttf"
            COMMAND ${SQL_GZIP} -9 -n -f "${work}/LiberationSans-${face}.ttf"
            COMMAND ${CMAKE_OBJCOPY} -I binary -O ${output_target} -B ${arch} LiberationSans-${face}.ttf.gz "${object}"
            COMMAND ${CMAKE_OBJCOPY} --add-section .note.GNU-stack=/dev/null
                    --set-section-flags .note.GNU-stack=noload,readonly "${object}"
            WORKING_DIRECTORY "${work}"
            DEPENDS "${font}"
            COMMENT "Embedding gzipped LiberationSans-${face}.ttf into ${target}"
            VERBATIM)
        set_source_files_properties("${object}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
        list(APPEND objects "${object}")
    endforeach()
    target_sources(${target} PRIVATE ${objects})
    target_compile_definitions(${target} PRIVATE SQL_EMBEDDED_FONTS=1)
endfunction()
