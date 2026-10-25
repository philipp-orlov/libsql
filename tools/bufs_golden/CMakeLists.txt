# Real BufsWriter output for the TypeScript reader's tests: prints ui/src/app/bufs/golden.ts. The ctest fails when
# the committed file differs, so the C++ writer and the TypeScript reader cannot drift apart. Regenerate with
#   <build>/tools/sql/bufs_golden/sql_bufs_golden > tools/sql/ui/src/app/bufs/golden.ts
add_executable(sql_bufs_golden main.cpp)
target_link_libraries(sql_bufs_golden PRIVATE sqlweb)
set(SQL_GOLDEN_TS ${CMAKE_CURRENT_SOURCE_DIR}/../sql/ui/src/app/bufs/golden.ts)
add_test(NAME bufs_golden_up_to_date
         COMMAND ${CMAKE_COMMAND} -DGENERATOR=$<TARGET_FILE:sql_bufs_golden> -DEXPECTED=${SQL_GOLDEN_TS}
                 -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/golden.generated.ts -P ${CMAKE_CURRENT_SOURCE_DIR}/compare.cmake)
