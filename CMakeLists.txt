cmake_minimum_required(VERSION 3.24)

get_filename_component(WPERF_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)
set(WPERF_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}")

set(WPERF_VERSION_MAJOR 0 CACHE STRING "wperf major version")
set(WPERF_VERSION_MINOR 12 CACHE STRING "wperf minor version")
set(WPERF_VERSION_PATCH 1 CACHE STRING "wperf patch version")
set(WPERF_BUILD_NUMBER 0 CACHE STRING "wperf build number")
foreach(part WPERF_VERSION_MAJOR WPERF_VERSION_MINOR WPERF_VERSION_PATCH WPERF_BUILD_NUMBER)
    set(version_value "${${part}}")
    if(NOT version_value MATCHES "^[0-9]+$")
        message(FATAL_ERROR "${part} must be a non-negative integer")
    endif()
endforeach()
set(WPERF_VERSION_BASE "${WPERF_VERSION_MAJOR}.${WPERF_VERSION_MINOR}.${WPERF_VERSION_PATCH}")
set(WPERF_VERSION_STRING "${WPERF_VERSION_BASE}.${WPERF_BUILD_NUMBER}")

project(wperf VERSION ${WPERF_VERSION_BASE} LANGUAGES CXX)

include(cmake/ProjectOptions.cmake)
include(cmake/WperfTargets.cmake)

enable_testing()
include(cmake/TestHelpers.cmake)
include(cmake/tests/CoreTests.cmake)
include(cmake/tests/CliTests.cmake)
include(cmake/tests/FixtureTests.cmake)
