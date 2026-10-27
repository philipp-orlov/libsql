cmake_minimum_required(VERSION 3.16)
project(libsql VERSION 0.2.0 LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "libsql requires a 64-bit target")
endif()

option(SQL_WITH_BUFS "sql::BufsWriter binary results (needs libbufs)" ON)
option(SQL_WITH_TLS "wss:// for Database::connect and https for the sql tool (needs OpenSSL; libhttp is built with HTTP_WITH_SSL)" ON)
option(SQL_WITH_REMOTE "Remote databases over WebSocket: Database::connect and sql::remote::SqlSocket (needs libhttp and SQL_WITH_BUFS)" ON)
option(SQL_BUILD_TESTS "Build the test suite" ON)
option(SQL_BUILD_EXAMPLES "Build the examples" ON)
option(SQL_BUILD_TOOLS "Build the sql command line tool" ON)
option(SQL_BUILD_BENCHMARKS "Build the benchmarks (see benchmarks/)" OFF)
option(SQL_BUILD_CONFORMANCE "Build the SQLite conformance harness (see tests/conformance/)" OFF)
option(SQL_ENABLE_ASAN "Build with address+undefined sanitizers" OFF)
# Experimental: the binding has not been through the security/robustness review
# the C++ library has. Never part of a product build.
option(SQL_BUILD_PYTHON "Build the Python binding (see python/README.md)" OFF)

set(SQL_NOSQL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../libnosql"
    CACHE PATH "Source directory of libnosql, when it is not already installed")

set(SQL_BUFS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../libbufs" CACHE PATH "Source directory of libbufs")
set(SQL_HTTP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../libhttp" CACHE PATH "Source directory of libhttp, when it is not already part of the build")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Link-time optimization for optimized builds: libsql calls into libnosql's
# cursor on every row, and only LTO can inline across the two libraries.
option(SQL_LTO "Link-time optimization in Release/RelWithDebInfo builds" OFF)
if(SQL_LTO AND NOT SQL_ENABLE_ASAN)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT sql_ipo_ok OUTPUT sql_ipo_msg LANGUAGES CXX)
    if(sql_ipo_ok)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
    else()
        message(STATUS "libsql: LTO not supported here: ${sql_ipo_msg}")
    endif()
endif()

# Profile-guided optimization (GCC/Clang). Build with GENERATE, run a
# representative workload (tools/pgo-build.sh runs the benchmarks), then
# rebuild the same tree with USE. Applies to libnosql as well.
set(SQL_PGO "" CACHE STRING "Profile-guided optimization: empty, GENERATE or USE")
set_property(CACHE SQL_PGO PROPERTY STRINGS "" GENERATE USE)
set(SQL_PGO_DIR "${CMAKE_BINARY_DIR}/pgo-data" CACHE PATH "Where PGO profiles are written and read")
if(SQL_PGO STREQUAL "GENERATE")
    add_compile_options(-fprofile-generate=${SQL_PGO_DIR} -fprofile-update=atomic)
    add_link_options(-fprofile-generate=${SQL_PGO_DIR})
elseif(SQL_PGO STREQUAL "USE")
    add_compile_options(-fprofile-use=${SQL_PGO_DIR} -fprofile-partial-training -Wno-missing-profile)
    add_link_options(-fprofile-use=${SQL_PGO_DIR})
elseif(NOT SQL_PGO STREQUAL "")
    message(FATAL_ERROR "SQL_PGO must be empty, GENERATE or USE")
endif()

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

# --- binary results -------------------------------------------------------
# libbufs is header-only plus the bufsc schema compiler, built from the sibling
# checkout. Its tests and benchmarks stay out of this build.
if(SQL_WITH_BUFS AND NOT TARGET bufs::bufs)
    if(NOT EXISTS "${SQL_BUFS_DIR}/CMakeLists.txt")
        message(FATAL_ERROR
            "libbufs not found. Point -DSQL_BUFS_DIR at it, or set -DSQL_WITH_BUFS=OFF.")
    endif()
    set(BUFS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(BUFS_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
    add_subdirectory("${SQL_BUFS_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/bufs")
endif()

# --- remote databases -----------------------------------------------------
# libhttp (WebSocket server routes and client) from the sibling checkout; the sql tool's web interface
# shares it. Its tests, examples and benchmarks stay out of this build.
if(SQL_WITH_REMOTE AND NOT SQL_WITH_BUFS)
    message(FATAL_ERROR "SQL_WITH_REMOTE needs SQL_WITH_BUFS (results travel as libbufs messages)")
endif()
if((SQL_WITH_REMOTE OR SQL_BUILD_TOOLS) AND NOT TARGET http)
    if(EXISTS "${SQL_HTTP_DIR}/CMakeLists.txt")
        set(HTTP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(HTTP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(HTTP_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
        if(SQL_WITH_TLS)
            find_package(OpenSSL 1.1.1 QUIET)
            if(OpenSSL_FOUND)
                set(HTTP_WITH_SSL ON CACHE BOOL "" FORCE)
            else()
                message(STATUS "OpenSSL not found: wss:// is unavailable (libhttp is built without TLS)")
            endif()
        endif()
        add_subdirectory("${SQL_HTTP_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/libhttp")
    elseif(SQL_WITH_REMOTE)
        message(FATAL_ERROR "libhttp not found. Point -DSQL_HTTP_DIR at it, or set -DSQL_WITH_REMOTE=OFF.")
    endif()
endif()

add_library(sql
    src/value.cpp
    src/database.cpp
    src/json.cpp
    src/internal/access.cpp
    src/internal/catalog.cpp
    src/internal/cnumeric.cpp
    src/internal/ddl.cpp
    src/internal/dml.cpp
    src/internal/encoding.cpp
    src/internal/eval.cpp
    src/internal/explain.cpp
    src/internal/func.cpp
    src/internal/func_aggregate.cpp
    src/internal/func_core.cpp
    src/internal/func_date.cpp
    src/internal/func_json.cpp
    src/internal/func_printf.cpp
    src/internal/lexer.cpp
    src/internal/parser.cpp
    src/internal/plan_select.cpp
    src/internal/pragma.cpp
    src/internal/pragma_storage.cpp
    src/internal/run_select.cpp
    src/internal/schema.cpp
    src/internal/statement.cpp
    src/internal/types.cpp
)
add_library(sql::sql ALIAS sql)

target_include_directories(sql
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(sql PUBLIC cxx_std_17)
target_link_libraries(sql PUBLIC nosql::nosql)

if(SQL_WITH_BUFS)
    target_sources(sql PRIVATE src/bufs.cpp)
    bufs_generate(sql SCHEMAS include/sql/wire/resultset.bufs include/sql/wire/remote.bufs
                  OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/gen/sql/wire)
    target_include_directories(sql PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/gen>)
    # bufs_generate links libbufs PRIVATE, and a static library re-exports that as LINK_ONLY, which an
    # install export would have to resolve. Readers of the generated header need libbufs PUBLIC
    # anyway. It has no install rules yet, so it is a build-tree dependency: an installed libsql
    # expects libbufs on the include path.
    set_property(TARGET sql PROPERTY INTERFACE_LINK_LIBRARIES nosql::nosql $<BUILD_INTERFACE:bufs::bufs>)
    target_compile_definitions(sql PUBLIC SQL_WITH_BUFS=1)
endif()

if(SQL_WITH_REMOTE)
    target_sources(sql PRIVATE src/remote/protocol.cpp src/remote/sql_socket.cpp src/remote/client.cpp)
    # Like libbufs, libhttp is a build-tree dependency: the plain LINK_LIBRARIES property builds sql itself against
    # it without putting it into the install export (a PRIVATE link of a static library would).
    set_property(TARGET sql APPEND PROPERTY LINK_LIBRARIES http)
    set_property(TARGET sql APPEND PROPERTY INTERFACE_LINK_LIBRARIES $<BUILD_INTERFACE:http>)
    target_compile_definitions(sql PUBLIC SQL_WITH_REMOTE=1)
else()
    target_sources(sql PRIVATE src/remote/client_stub.cpp)
endif()

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
if(SQL_WITH_BUFS)
    install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/gen/sql/wire
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/sql FILES_MATCHING PATTERN "*.hpp")
endif()
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
# Before the benchmarks: bench_vs_sqlite links the conformance oracle.
if(SQL_BUILD_CONFORMANCE)
    enable_testing()
    add_subdirectory(tests/conformance)
endif()
if(SQL_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
if(SQL_BUILD_PYTHON)
    add_subdirectory(python)
endif()
