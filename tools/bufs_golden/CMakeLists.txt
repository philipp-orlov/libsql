# Real BufsWriter output for the TypeScript reader's tests: prints ui/src/app/bufs/golden.ts. The ctest fails when
# the committed file differs, so the C++ writer and the TypeScript reader cannot drift apart. Regenerate with
#   <build>/tools/sql/bufs_golden/sql_bufs_golden > tools/sql/ui/src/app/bufs/golden.ts
add_executable(sql_bufs_golden main.cpp remote.cpp)
target_include_directories(sql_bufs_golden PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(sql_bufs_golden PRIVATE sql http)
set(SQL_GOLDEN_TS ${CMAKE_CURRENT_SOURCE_DIR}/../sql/ui/src/app/bufs/golden.ts)
add_test(NAME bufs_golden_up_to_date
         COMMAND ${CMAKE_COMMAND} -DGENERATOR=$<TARGET_FILE:sql_bufs_golden> -DEXPECTED=${SQL_GOLDEN_TS}
                 -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/golden.generated.ts -P ${CMAKE_CURRENT_SOURCE_DIR}/compare.cmake)

# The protocol's requests and replies for the TypeScript encoder and decoder (tools/sql/ui/src/app/remote).
set(SQL_REMOTE_GOLDEN_TS ${CMAKE_CURRENT_SOURCE_DIR}/../sql/ui/src/app/remote/golden.ts)
add_test(NAME remote_golden_up_to_date
         COMMAND ${CMAKE_COMMAND} -DGENERATOR=$<TARGET_FILE:sql_bufs_golden> -DARGS=--remote -DEXPECTED=${SQL_REMOTE_GOLDEN_TS}
                 -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/remote-golden.generated.ts -P ${CMAKE_CURRENT_SOURCE_DIR}/compare.cmake)
