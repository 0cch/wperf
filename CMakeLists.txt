cmake_minimum_required(VERSION 3.24)

set(WPERF_VERSION_MAJOR 0 CACHE STRING "wperf major version")
set(WPERF_VERSION_MINOR 8 CACHE STRING "wperf minor version")
set(WPERF_VERSION_PATCH 0 CACHE STRING "wperf patch version")
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

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "ProgramDatabase")
add_compile_options(/Zi)
add_link_options(/DEBUG)

option(WPERF_ENABLE_DISASM "Build optional Zydis-based disassembly support" ON)

include(FetchContent)
set(CLI11_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_DOCS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    cli11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG v2.7.2
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(cli11)

if(WPERF_ENABLE_DISASM)
    set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(ZYDIS_BUILD_MAN OFF CACHE BOOL "" FORCE)
    set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)
    set(ZYDIS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(ZYDIS_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
    set(ZYDIS_FEATURE_ENCODER OFF CACHE BOOL "" FORCE)
    set(ZYDIS_FEATURE_FORMATTER ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
        zycore
        GIT_REPOSITORY https://github.com/zyantific/zycore-c.git
        GIT_TAG 0b2432ced0884fd152b471d97ecf0258ff4d859f
        GIT_SHALLOW FALSE
    )
    FetchContent_GetProperties(zycore)
    if(NOT zycore_POPULATED)
        FetchContent_Populate(zycore)
    endif()
    set(ZYAN_ZYCORE_PATH "${zycore_SOURCE_DIR}" CACHE PATH "" FORCE)
    set(ZYAN_SYSTEM_ZYCORE OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        zydis
        GIT_REPOSITORY https://github.com/zyantific/zydis.git
        GIT_TAG v4.1.1
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(zydis)
endif()

add_library(wperf_core STATIC
    src/common.cpp
    src/archive.cpp
    src/analysis.cpp
    src/logger.cpp
    src/state.cpp
    src/etw.cpp
    src/etw_decode.cpp
    src/session_guard.cpp
    src/ipc.cpp
    src/elevation.cpp
    src/crash.cpp
    src/trace_model.cpp
    src/trace_events.cpp
    src/lbr.cpp
    src/disassembler.cpp
    src/etl_reader.cpp
    src/live_top.cpp
    src/live_data.cpp
    src/top_renderer.cpp
    src/commands/live_control.cpp
    src/symbolizer.cpp
    src/cpu_report.cpp
    src/off_cpu_report.cpp
)
target_include_directories(wperf_core PUBLIC src)
target_compile_definitions(wperf_core PUBLIC
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
    WPERF_VERSION_STRING="${WPERF_VERSION_STRING}"
)
target_compile_options(wperf_core PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_link_libraries(wperf_core PUBLIC advapi32 bcrypt dbghelp shell32 psapi tdh ole32)
if(WPERF_ENABLE_DISASM)
    target_compile_definitions(wperf_core PUBLIC WPERF_HAVE_ZYDIS=1)
    target_link_libraries(wperf_core PUBLIC Zydis)
endif()

add_executable(wperf
    src/main.cpp
    src/dbghelp_loader.cpp
    src/commands.cpp
    src/cli.cpp
    src/commands/advanced.cpp
    src/commands/offline.cpp
    src/commands/live_worker.cpp
    src/commands/top.cpp
)
target_compile_options(wperf PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_link_options(wperf PRIVATE /DELAYLOAD:dbghelp.dll)
target_link_libraries(wperf PRIVATE wperf_core CLI11::CLI11 delayimp)
add_custom_target(generate-command-reference
    COMMAND pwsh -NoProfile -File ${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_command_reference.ps1
            -Wperf $<TARGET_FILE:wperf>
            -OutputPath ${CMAKE_CURRENT_SOURCE_DIR}/docs/COMMAND_REFERENCE.md
    DEPENDS wperf
    COMMENT "Generating docs/COMMAND_REFERENCE.md from the wperf command catalog")
add_executable(wperf_cpu_burn tests/workloads/cpu_burn.cpp)
target_compile_definitions(wperf_cpu_burn PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(wperf_cpu_burn PRIVATE /W4 /permissive- /utf-8 /EHsc)

enable_testing()

add_executable(wperf_unit_tests tests/unit_tests.cpp src/dbghelp_loader.cpp)
target_compile_options(wperf_unit_tests PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_compile_definitions(wperf_unit_tests PRIVATE
    WPERF_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
target_link_libraries(wperf_unit_tests PRIVATE wperf_core)
add_test(NAME wperf_unit_tests COMMAND wperf_unit_tests)
add_executable(wperf_cli_tests tests/cli_unit_tests.cpp src/cli.cpp)
target_compile_options(wperf_cli_tests PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_link_libraries(wperf_cli_tests PRIVATE wperf_core CLI11::CLI11)
add_test(NAME cli_unit_tests COMMAND wperf_cli_tests)
add_executable(wperf_regression_tests tests/regression_tests.cpp)
target_compile_options(wperf_regression_tests PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_link_libraries(wperf_regression_tests PRIVATE wperf_core)

target_compile_definitions(wperf_regression_tests PRIVATE WPERF_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
add_test(NAME wperf_regression_tests COMMAND wperf_regression_tests)
set_tests_properties(wperf_regression_tests PROPERTIES TIMEOUT 20)
add_executable(wperf_live_integration_tests tests/live_integration_tests.cpp)
target_compile_options(wperf_live_integration_tests PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_link_libraries(wperf_live_integration_tests PRIVATE wperf_core)
add_test(NAME live_integration COMMAND wperf_live_integration_tests)
set_tests_properties(live_integration PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE SKIP_RETURN_CODE 77 LABELS integration)
add_test(NAME live_top_controller_smoke
         COMMAND pwsh -NoProfile -File ${CMAKE_CURRENT_SOURCE_DIR}/tests/live_top_smoke.ps1
         -Wperf $<TARGET_FILE:wperf>)
set_tests_properties(live_top_controller_smoke PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE
                     SKIP_RETURN_CODE 77 LABELS integration)
add_test(NAME cli_version COMMAND wperf --version)
add_test(NAME cli_version_format COMMAND pwsh -NoProfile -File ${CMAKE_CURRENT_SOURCE_DIR}/tests/version_test.ps1 -Wperf $<TARGET_FILE:wperf>)
add_test(NAME cli_list COMMAND wperf list)
add_test(NAME cli_doctor COMMAND wperf doctor)
add_test(NAME cli_doctor_help COMMAND wperf doctor --help)
add_test(NAME cli_help_report COMMAND wperf help report)
add_test(NAME cli_report_help COMMAND wperf report --help)
add_test(NAME cli_buildid_help COMMAND wperf buildid-list --help)
add_test(NAME cli_trace_help COMMAND wperf trace --help)
add_test(NAME cli_sched_help COMMAND wperf sched --help)
add_test(NAME cli_stat_help COMMAND wperf stat --help)
add_test(NAME cli_top_help COMMAND wperf top --help)
add_test(NAME cli_script_help COMMAND wperf script --help)
add_test(NAME cli_annotate_help COMMAND wperf annotate --help)
add_test(NAME cli_diff_help COMMAND wperf diff --help)
add_test(NAME cli_archive_help COMMAND wperf archive --help)
add_test(NAME cli_record_help COMMAND wperf record --help)
add_test(NAME cli_report_io COMMAND wperf report -i ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/io.etl --kind io)
add_test(NAME cli_report_io_json COMMAND wperf report -i ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/io.etl --kind io --json)
add_test(NAME cli_folded_fixture COMMAND wperf report -i ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/cpu-stack.etl --folded --percent-limit 5)
add_test(NAME cli_folded_output COMMAND pwsh -NoProfile -File ${CMAKE_CURRENT_SOURCE_DIR}/tests/folded_output_test.ps1 -Wperf $<TARGET_FILE:wperf> -Fixture ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/cpu-stack.etl -SymbolPath $<TARGET_FILE_DIR:wperf> -OutputDirectory ${CMAKE_CURRENT_BINARY_DIR})
add_test(NAME cli_off_cpu_fixture COMMAND wperf report -i ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/sched.etl --off-cpu --folded --percent-limit 0)
add_test(NAME cli_off_cpu_json COMMAND wperf report -i ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/sched.etl --off-cpu --json --percent-limit 0)
add_test(NAME cli_off_cpu_folded_output
         COMMAND pwsh -NoProfile -File ${CMAKE_CURRENT_SOURCE_DIR}/tests/folded_output_test.ps1
         -Wperf $<TARGET_FILE:wperf> -Fixture ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/sched.etl
         -SymbolPath $<TARGET_FILE_DIR:wperf> -OutputDirectory ${CMAKE_CURRENT_BINARY_DIR} -OffCpu)
add_test(NAME cli_off_cpu_missing_gate
         COMMAND wperf report -i ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/sched.etl --off-cpu --json --percent-limit 100 --max-missing-stacks 0)
set_tests_properties(cli_off_cpu_missing_gate PROPERTIES WILL_FAIL TRUE)
add_test(NAME cli_off_cpu_strict_gate
         COMMAND wperf report -i ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/sched.etl --off-cpu --json --percent-limit 100 --strict-quality)
set_tests_properties(cli_off_cpu_strict_gate PROPERTIES WILL_FAIL TRUE)
add_test(NAME cli_report_conflicting_formats COMMAND wperf report -i ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/cpu-stack.etl --json --csv)
add_test(NAME cli_annotate_conflicting_formats COMMAND wperf annotate -i ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/cpu-stack.etl --json --csv)
add_test(NAME cli_sched_fixture COMMAND wperf sched -i ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/sched.etl)
add_test(NAME cli_syscall_fixture COMMAND wperf trace -i ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/syscall.etl -e syscall --max 10)
add_test(NAME cli_script_fixture COMMAND wperf script -i ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/cpu-stack.etl --limit 3)
add_test(NAME cli_script_csv COMMAND wperf script -i ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/cpu-stack.etl --limit 3 -F timestamp,pid,comm,symbol --csv)
add_test(NAME cli_script_missing_value COMMAND wperf script -i)
add_test(NAME cli_trace_missing_value COMMAND wperf trace -e)
add_test(NAME archive_manifest_test COMMAND pwsh -NoProfile -File ${CMAKE_CURRENT_SOURCE_DIR}/scripts/archive_manifest_test.ps1 -Wperf $<TARGET_FILE:wperf> -Fixture ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/cpu-stack.etl -OutputDirectory ${CMAKE_CURRENT_BINARY_DIR})
add_test(NAME cli_provider_list COMMAND wperf list providers --max 1)
add_test(NAME cli_provider_bad_guid COMMAND wperf record --provider not-a-guid)
add_test(NAME replay_contract COMMAND pwsh -NoProfile -File ${CMAKE_CURRENT_SOURCE_DIR}/scripts/replay_contract.ps1
         -Wperf $<TARGET_FILE:wperf> -Fixture ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/cpu-stack.etl
         -SymbolPath $<TARGET_FILE_DIR:wperf>)
add_test(NAME json_contract_test
         COMMAND pwsh -NoProfile -File ${CMAKE_CURRENT_SOURCE_DIR}/tests/json_contract_test.ps1
         -Wperf $<TARGET_FILE:wperf>
         -FixtureDirectory ${CMAKE_CURRENT_SOURCE_DIR}/tests/data
         -SchemaDirectory ${CMAKE_CURRENT_SOURCE_DIR}/docs/schemas)
set_tests_properties(json_contract_test PROPERTIES TIMEOUT 60)
add_test(NAME cli_conformance_test
         COMMAND pwsh -NoProfile -File ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_conformance_test.ps1
         -Wperf $<TARGET_FILE:wperf>
         -ReferencePath ${CMAKE_CURRENT_SOURCE_DIR}/docs/COMMAND_REFERENCE.md)
set_tests_properties(cli_conformance_test PROPERTIES TIMEOUT 60)
add_test(NAME cli_pmc_frequency_rejected COMMAND wperf record --pmc-source 19 -F 999)
add_test(NAME cli_report_nan_rejected COMMAND wperf report --percent-limit nan)
add_test(NAME cli_negative_limit_rejected COMMAND wperf script --limit -1)
set_tests_properties(cli_provider_bad_guid cli_report_conflicting_formats
                     cli_annotate_conflicting_formats
                     cli_script_missing_value cli_trace_missing_value
                     cli_pmc_frequency_rejected cli_report_nan_rejected cli_negative_limit_rejected
                     PROPERTIES WILL_FAIL TRUE)
