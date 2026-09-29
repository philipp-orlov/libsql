add_executable(sql_cli sql_tool.cpp)
target_link_libraries(sql_cli PRIVATE sql)
set_target_properties(sql_cli PROPERTIES OUTPUT_NAME sql)
install(TARGETS sql_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
