cmake_minimum_required(VERSION 3.16)
project(libsql VERSION 0.1.0 LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "libsql requires a 64-bit target")
endif()

option(SQL_BUILD_TESTS "Build the test suite" ON)
option(SQL_BUILD_EXAMPLES "Build the examples" ON)
option(SQL_BUILD_TOOLS "Build the sql command line tool" ON)
option(SQL_BUILD_BENCHMARKS "Build the benchmarks (see benchmarks/)" OFF)
option(SQL_ENABLE_ASAN "Build with address+undefined sanitizers" OFF)
# Experimental: the binding has not been through the security/robustness review
# the C++ library has. Never part of a product build.
option(SQL_BUILD_PYTHON "Build the Python binding (see python/README.md)" OFF)

set(SQL_NOSQL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../libnosql"
    CACHE PATH "Source directory of libnosql, when it is not already installed")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# An extension module is a shared object, so everything linked into it --
# libnosql included, which is added below -- has to be position independent.
if(SQL_BUILD_PYTHON)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

# --- the storage engine ---------------------------------------------------
# Prefer an installed libnosql; fall back to the sibling checkout. Its own
# tests and samples stay out of this build either way.
if(NOT TARGET nosql::nosql)
    find_package(nosql QUIET)
endif()
if(NOT TARGET nosql::nosql)
    if(NOT EXISTS "${SQL_NOSQL_DIR}/CMakeLists.txt")
        message(FATAL_ERROR
            "libnosql not found. Install it, or point -DSQL_NOSQL_DIR at its source tree.")
    endif()
    set(NOSQL_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(NOSQL_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(NOSQL_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(NOSQL_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
    # libsql itself needs only the core store. The blob archive is compiled in
    # for the tests and the Python binding, which exercise it; the message
    # queue and replication are never needed. Set them explicitly to override.
    if(SQL_BUILD_TESTS OR SQL_BUILD_PYTHON)
        set(NOSQL_WITH_BLOB ON CACHE BOOL "Build libnosql's BlobStorage")
    else()
        set(NOSQL_WITH_BLOB OFF CACHE BOOL "Build libnosql's BlobStorage")
    endif()
    set(NOSQL_WITH_MQ OFF CACHE BOOL "Build libnosql's message queue")
    set(NOSQL_WITH_REPLICATION OFF CACHE BOOL "Build libnosql's replication")
    add_subdirectory("${SQL_NOSQL_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/nosql")
endif()

add_library(sql
    src/value.cpp
    src/database.cpp
    src/json.cpp
    src/internal/lexer.cpp
    src/internal/cnumeric.cpp
    src/internal/parser.cpp
    src/internal/encoding.cpp
    src/internal/catalog.cpp
    src/internal/executor.cpp
)
add_library(sql::sql ALIAS sql)

target_include_directories(sql
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(sql PUBLIC cxx_std_17)
target_link_libraries(sql PUBLIC nosql::nosql)

if(MSVC)
    target_compile_options(sql PRIVATE /W4 /permissive- /Zc:__cplusplus)
    target_compile_definitions(sql PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    target_compile_options(sql PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
endif()

if(SQL_ENABLE_ASAN AND NOT MSVC)
    target_compile_options(sql PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer -g)
    target_link_options(sql PUBLIC -fsanitize=address,undefined)
endif()

set_target_properties(sql PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    OUTPUT_NAME sql
)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS sql EXPORT sql-targets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/sql DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT sql-targets NAMESPACE sql:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sql)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/sqlConfigVersion.cmake"
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/sqlConfig.cmake"
    "include(CMakeFindDependencyMacro)\n"
    "find_dependency(nosql)\n"
    "include(\"\${CMAKE_CURRENT_LIST_DIR}/sql-targets.cmake\")\n")
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/sqlConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/sqlConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sql)

if(SQL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
if(SQL_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
if(SQL_BUILD_TOOLS)
    add_subdirectory(tools)
endif()
if(SQL_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
if(SQL_BUILD_PYTHON)
    add_subdirectory(python)
endif()
