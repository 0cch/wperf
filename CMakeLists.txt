cmake_minimum_required(VERSION 3.24)

project(wperf VERSION 0.5.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "ProgramDatabase")
add_compile_options(/Zi)
add_link_options(/DEBUG)

add_library(wperf_core STATIC
    src/common.cpp
    src/logger.cpp
    src/state.cpp
    src/etw.cpp
    src/ipc.cpp
    src/elevation.cpp
    src/crash.cpp
    src/trace_model.cpp
    src/trace_events.cpp
    src/lbr.cpp
    src/etl_reader.cpp
    src/live_top.cpp
    src/commands/live_control.cpp
    src/symbolizer.cpp
    src/cpu_report.cpp
)
target_include_directories(wperf_core PUBLIC src)
target_compile_definitions(wperf_core PUBLIC
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
)
target_compile_options(wperf_core PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_link_libraries(wperf_core PUBLIC advapi32 bcrypt dbghelp shell32 psapi tdh ole32)

add_executable(wperf
    src/main.cpp
    src/commands.cpp
    src/commands/advanced.cpp
    src/commands/offline.cpp
    src/commands/live_worker.cpp
    src/commands/top.cpp
)
target_compile_options(wperf PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_link_libraries(wperf PRIVATE wperf_core)

add_executable(wperf_cpu_burn tests/workloads/cpu_burn.cpp)
target_compile_definitions(wperf_cpu_burn PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(wperf_cpu_burn PRIVATE /W4 /permissive- /utf-8 /EHsc)

enable_testing()

add_executable(wperf_unit_tests tests/unit_tests.cpp)
target_compile_options(wperf_unit_tests PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_compile_definitions(wperf_unit_tests PRIVATE
    WPERF_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
target_link_libraries(wperf_unit_tests PRIVATE wperf_core)
add_test(NAME wperf_unit_tests COMMAND wperf_unit_tests)
add_test(NAME cli_version COMMAND wperf --version)
add_test(NAME cli_list COMMAND wperf list)
add_test(NAME cli_doctor COMMAND wperf doctor)
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
add_test(NAME cli_sched_fixture COMMAND wperf sched -i ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/sched.etl)
add_test(NAME cli_syscall_fixture COMMAND wperf trace -i ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/syscall.etl -e syscall --max 10)
add_test(NAME archive_manifest_smoke COMMAND pwsh -NoProfile -File ${CMAKE_CURRENT_SOURCE_DIR}/scripts/archive_smoke.ps1 -Wperf $<TARGET_FILE:wperf> -Fixture ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/cpu-stack.etl -OutputDirectory ${CMAKE_CURRENT_BINARY_DIR})
add_test(NAME cli_provider_list COMMAND wperf list providers --max 1)
add_test(NAME cli_provider_bad_guid COMMAND wperf record --provider not-a-guid)
set_tests_properties(cli_provider_bad_guid PROPERTIES WILL_FAIL TRUE)
