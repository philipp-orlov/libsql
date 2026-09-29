# The Python binding: one extension module, `libsql._libsql`, plus the pure
# Python package around it.
#
# Three ways in:
#   * as part of libsql:  cmake -S . -B build -DSQL_BUILD_PYTHON=ON
#   * on its own:         cmake -S python -B build-py   (needs the ../libsql, ../../libnosql checkouts)
#   * through pip:        pip install ./python        (pyproject.toml drives this file)
#
# The result lands in <build>/python/libsql, so
#   PYTHONPATH=<build>/python python -c "import libsql"
# works straight out of the build tree.
cmake_minimum_required(VERSION 3.18)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    # Standalone or pip: pull the engine in either from _vendor (see vendor.sh --
    # this is what makes python/ buildable on its own, e.g. in a manylinux
    # container that never sees the sibling checkouts) or, failing that, from
    # the directories above, for a build in the full source tree.
    project(libsql_python LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
    endif()
    set(SQL_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(SQL_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SQL_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(SQL_BUILD_PYTHON OFF CACHE BOOL "" FORCE)  # this file *is* the python part
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/_vendor/libsql/CMakeLists.txt)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/_vendor/libsql ${CMAKE_BINARY_DIR}/libsql)
    else()
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_BINARY_DIR}/libsql)
    endif()
endif()

find_package(Python 3.8 COMPONENTS Interpreter Development.Module REQUIRED)

# pybind11: an installed CMake package, else the one inside a pip-installed
# wheel, else a pinned checkout.
find_package(pybind11 CONFIG QUIET)
if(NOT pybind11_FOUND)
    execute_process(COMMAND ${Python_EXECUTABLE} -m pybind11 --cmakedir
                    OUTPUT_VARIABLE _pybind11_dir OUTPUT_STRIP_TRAILING_WHITESPACE
                    RESULT_VARIABLE _pybind11_rc ERROR_QUIET)
    if(_pybind11_rc EQUAL 0)
        find_package(pybind11 CONFIG QUIET PATHS ${_pybind11_dir} NO_DEFAULT_PATH)
    endif()
endif()
if(NOT pybind11_FOUND)
    message(STATUS "pybind11 not installed; fetching v3.1.0")
    include(FetchContent)
    FetchContent_Declare(pybind11
        GIT_REPOSITORY https://github.com/pybind/pybind11.git
        GIT_TAG        v3.1.0
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(pybind11)
endif()

pybind11_add_module(_libsql MODULE src/module.cpp)
target_link_libraries(_libsql PRIVATE sql::sql)
if(MSVC)
    target_compile_options(_libsql PRIVATE /W4 /permissive- /Zc:__cplusplus)
else()
    target_compile_options(_libsql PRIVATE -Wall -Wextra -Wshadow)
endif()

set(SQL_PYTHON_PACKAGE_DIR ${CMAKE_BINARY_DIR}/python/libsql)
set_target_properties(_libsql PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${SQL_PYTHON_PACKAGE_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${SQL_PYTHON_PACKAGE_DIR})

# The pure-Python half goes beside the module so the build tree is importable.
file(GLOB SQL_PYTHON_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/libsql/*.py)
add_custom_command(TARGET _libsql POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SQL_PYTHON_PACKAGE_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SQL_PYTHON_SOURCES} ${SQL_PYTHON_PACKAGE_DIR}
    COMMENT "Assembling the libsql Python package in ${SQL_PYTHON_PACKAGE_DIR}")

if(SKBUILD)
    # pip build: the module goes into the wheel's libsql/ package directory.
    install(TARGETS _libsql LIBRARY DESTINATION libsql RUNTIME DESTINATION libsql)
endif()
