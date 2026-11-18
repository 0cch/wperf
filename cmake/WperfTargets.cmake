find_package(CLI11 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
if(WPERF_ENABLE_DISASM)
    find_package(Zydis CONFIG REQUIRED)
endif()

if(WPERF_WARNINGS_AS_ERRORS)
    add_compile_options(/WX)
endif()

function(wperf_enable_cxx_options target)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /EHsc)
endfunction()

add_library(wperf_core STATIC
    src/common.cpp
    src/json_writer.cpp
    src/requests.cpp
    src/archive.cpp
    src/analysis.cpp
    src/logger.cpp
    src/state.cpp
    src/etw.cpp
    src/etw_decode.cpp
    src/diagnostic_decode.cpp
    src/diagnostic_report.cpp
    src/heap_metadata.cpp
    src/pool_tags.cpp
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
    src/flamegraph.cpp
    src/flamegraph_html.cpp
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
wperf_enable_cxx_options(wperf_core)
target_link_libraries(wperf_core PUBLIC advapi32 bcrypt dbghelp shell32 psapi tdh ole32 version nlohmann_json::nlohmann_json)
if(WPERF_ENABLE_DISASM)
    target_compile_definitions(wperf_core PUBLIC WPERF_HAVE_ZYDIS=1)
    target_link_libraries(wperf_core PUBLIC Zydis::Zydis)
endif()

add_executable(wperf
    src/main.cpp
    src/dbghelp_loader.cpp
    src/commands.cpp
    src/cli.cpp
    src/commands/advanced.cpp
    src/commands/offline.cpp
    src/commands/diagnostics.cpp
    src/commands/live_worker.cpp
    src/commands/top.cpp
)
wperf_enable_cxx_options(wperf)
target_link_options(wperf PRIVATE /DELAYLOAD:dbghelp.dll)
target_link_libraries(wperf PRIVATE wperf_core CLI11::CLI11 delayimp)
add_custom_target(generate-command-reference
    COMMAND pwsh -NoProfile -File ${WPERF_SOURCE_DIR}/scripts/generate_command_reference.ps1
            -Wperf $<TARGET_FILE:wperf>
            -OutputPath ${WPERF_SOURCE_DIR}/docs/COMMAND_REFERENCE.md
    DEPENDS wperf
    COMMENT "Generating docs/COMMAND_REFERENCE.md from the wperf command catalog")

add_executable(wperf_cpu_burn tests/workloads/cpu_burn.cpp)
target_compile_definitions(wperf_cpu_burn PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
wperf_enable_cxx_options(wperf_cpu_burn)
add_executable(wperf_diagnostic_workload tests/workloads/diagnostic_workload.cpp)
target_compile_definitions(wperf_diagnostic_workload PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
wperf_enable_cxx_options(wperf_diagnostic_workload)
add_executable(wperf_etw_fixture_workload tests/workloads/etw_fixture_workload.cpp)
target_compile_definitions(wperf_etw_fixture_workload PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
wperf_enable_cxx_options(wperf_etw_fixture_workload)
