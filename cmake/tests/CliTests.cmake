add_test(NAME cli_version COMMAND wperf --version)
wperf_add_pwsh_test(cli_version_format tests/version_test.ps1 -Wperf $<TARGET_FILE:wperf>)
add_test(NAME cli_list COMMAND wperf list)
add_test(NAME cli_doctor COMMAND wperf doctor)
add_test(NAME cli_doctor_help COMMAND wperf doctor --help)
add_test(NAME cli_help_report COMMAND wperf help report)
add_test(NAME cli_report_help COMMAND wperf report --help)
add_test(NAME cli_flamegraph_help COMMAND wperf flamegraph --help)
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

wperf_add_pwsh_test(cli_flamegraph_output tests/flamegraph_output_test.ps1
    -Wperf $<TARGET_FILE:wperf> -OutputDirectory ${WPERF_BINARY_DIR}/flamegraph-output)
wperf_set_test_timeout(30 cli_flamegraph_output)
find_program(WPERF_NODE_EXECUTABLE NAMES node)
if(WPERF_NODE_EXECUTABLE)
    add_test(NAME cli_flamegraph_browser_contract
             COMMAND ${WPERF_NODE_EXECUTABLE} ${WPERF_SOURCE_DIR}/tests/flamegraph_browser_contract_test.js
                     $<TARGET_FILE:wperf>)
endif()

add_test(NAME cli_provider_list COMMAND wperf list providers --max 1)
add_test(NAME cli_provider_bad_guid COMMAND wperf record --provider not-a-guid)
add_test(NAME cli_script_missing_value COMMAND wperf script -i)
add_test(NAME cli_trace_missing_value COMMAND wperf trace -e)
add_test(NAME cli_pmc_frequency_rejected COMMAND wperf record --pmc-source 19 -F 999)
add_test(NAME cli_report_nan_rejected COMMAND wperf report --percent-limit nan)
add_test(NAME cli_negative_limit_rejected COMMAND wperf script --limit -1)
wperf_add_pwsh_test(exit_code_test tests/exit_code_test.ps1 -Wperf $<TARGET_FILE:wperf>)
wperf_set_test_timeout(30 exit_code_test)
wperf_add_pwsh_test(agent_friendly_test tests/agent_friendly_test.ps1
    -Wperf $<TARGET_FILE:wperf> -SchemaDirectory ${WPERF_SOURCE_DIR}/docs/schemas)
wperf_set_test_timeout(30 agent_friendly_test)
wperf_add_pwsh_test(cli_conformance_test tests/cli_conformance_test.ps1
    -Wperf $<TARGET_FILE:wperf> -ReferencePath ${WPERF_SOURCE_DIR}/docs/COMMAND_REFERENCE.md)
wperf_set_test_timeout(60 cli_conformance_test)
wperf_add_pwsh_test(stat_output_test tests/stat_output_test.ps1 -Wperf $<TARGET_FILE:wperf>)
wperf_set_test_timeout(30 stat_output_test)

set_tests_properties(cli_provider_bad_guid cli_script_missing_value cli_trace_missing_value
    cli_pmc_frequency_rejected cli_report_nan_rejected cli_negative_limit_rejected
    PROPERTIES WILL_FAIL TRUE)
