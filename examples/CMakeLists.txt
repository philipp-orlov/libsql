# The targets keep the example_ prefix so they cannot collide with those of a
# project that adds this one as a subdirectory; the binaries do not.
foreach(name quickstart blobs json)
    add_executable(example_${name} ${name}.cpp)
    target_link_libraries(example_${name} PRIVATE sql)
    set_target_properties(example_${name} PROPERTIES OUTPUT_NAME ${name})
endforeach()

if(SQL_WITH_BUFS)
    add_executable(example_bufs bufs.cpp)
    target_link_libraries(example_bufs PRIVATE sql)
    set_target_properties(example_bufs PROPERTIES OUTPUT_NAME bufs)
endif()
