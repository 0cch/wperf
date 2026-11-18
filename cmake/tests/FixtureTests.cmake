wperf_add_pwsh_test(diagnostic_contract_test tests/diagnostic_contract_test.ps1
    -Tests $<TARGET_FILE:wperf_diagnostic_tests> -Wperf $<TARGET_FILE:wperf>
    -SchemaDirectory ${WPERF_SOURCE_DIR}/docs/schemas -Fixture ${WPERF_GENERATED_ETW_DIR}/cpu-stack.etl)
wperf_set_test_timeout(90 diagnostic_contract_test)

wperf_add_pwsh_test(cli_report_io tests/run_fixture_case.ps1
    -Wperf $<TARGET_FILE:wperf> -Fixture ${WPERF_GENERATED_ETW_DIR}/io.etl -Command report -ApplyPid --kind io)
wperf_add_pwsh_test(cli_report_io_json tests/run_fixture_case.ps1
    -Wperf $<TARGET_FILE:wperf> -Fixture ${WPERF_GENERATED_ETW_DIR}/io.etl -Command report -ApplyPid --kind io --json)
wperf_add_pwsh_test(cli_folded_fixture tests/run_fixture_case.ps1
    -Wperf $<TARGET_FILE:wperf> -Fixture ${WPERF_GENERATED_ETW_DIR}/cpu-stack.etl -Command report -ApplyPid --folded --percent-limit 5)
wperf_add_pwsh_test(cli_folded_output tests/folded_output_test.ps1
    -Wperf $<TARGET_FILE:wperf> -Fixture ${WPERF_GENERATED_ETW_DIR}/cpu-stack.etl -SymbolPath $<TARGET_FILE_DIR:wperf> -OutputDirectory ${WPERF_BINARY_DIR})
wperf_add_pwsh_test(cli_report_flamegraph_cpu tests/report_flamegraph_test.ps1
    -Wperf $<TARGET_FILE:wperf> -Fixture ${WPERF_GENERATED_ETW_DIR}/cpu-stack.etl
    -SymbolPath $<TARGET_FILE_DIR:wperf> -OutputDirectory ${WPERF_BINARY_DIR}/report-flamegraph)
wperf_add_pwsh_test(cli_report_flamegraph_offcpu tests/report_flamegraph_test.ps1
    -Wperf $<TARGET_FILE:wperf> -Fixture ${WPERF_GENERATED_ETW_DIR}/sched.etl
    -SymbolPath $<TARGET_FILE_DIR:wperf> -OutputDirectory ${WPERF_BINARY_DIR}/report-flamegraph -OffCpu)
wperf_add_pwsh_test(cli_off_cpu_fixture tests/run_fixture_case.ps1
    -Wperf $<TARGET_FILE:wperf> -Fixture ${WPERF_GENERATED_ETW_DIR}/sched.etl -Command report -ApplyPid --off-cpu --folded --percent-limit 0)
wperf_add_pwsh_test(cli_off_cpu_json tests/run_fixture_case.ps1
    -Wperf $<TARGET_FILE:wperf> -Fixture ${WPERF_GENERATED_ETW_DIR}/sched.etl -Command report -ApplyPid --off-cpu --json --percent-limit 0)
wperf_add_pwsh_test(cli_off_cpu_folded_output tests/folded_output_test.ps1
    -Wperf $<TARGET_FILE:wperf> -Fixture ${WPERF_GENERATED_ETW_DIR}/sched.etl -SymbolPath $<TARGET_FILE_DIR:wperf> -OutputDirectory ${WPERF_BINARY_DIR} -OffCpu)
wperf_add_pwsh_test(cli_off_cpu_missing_gate tests/run_fixture_case.ps1
    -Wperf $<TARGET_FILE:wperf> -Fixture ${WPERF_GENERATED_ETW_DIR}/sched.etl -Command report -ApplyPid --off-cpu --json --percent-limit 100 --max-missing-stacks 0)
wperf_add_pwsh_test(cli_off_cpu_strict_gate tests/run_fixture_case.ps1
    -Wperf $<TARGET_FILE:wperf> -Fixture ${WPERF_GENERATED_ETW_DIR}/sched.etl -Command report -ApplyPid --off-cpu --json --percent-limit 100 --strict-quality)
wperf_add_pwsh_test(cli_report_conflicting_formats tests/run_fixture_case.ps1
    -Wperf $<TARGET_FILE:wperf> -Fixture ${WPERF_GENERATED_ETW_DIR}/cpu-stack.etl -Command report -ApplyPid --json --csv)
wperf_add_pwsh_test(cli_annotate_conflicting_formats tests/run_fixture_case.ps1
    -Wperf $<TARGET_FILE:wperf> -Fixture ${WPERF_GENERATED_ETW_DIR}/cpu-stack.etl -Command annotate -ApplyPid --json --csv)
wperf_add_pwsh_test(cli_sched_fixture tests/run_fixture_case.ps1
    -Wperf $<TARGET_FILE:wperf> -Fixture ${WPERF_GENERATED_ETW_DIR}/sched.etl -Command sched)
wperf_add_pwsh_test(cli_syscall_fixture tests/run_fixture_case.ps1
    -Wperf $<TARGET_FILE:wperf> -Fixture ${WPERF_GENERATED_ETW_DIR}/syscall.etl -Command trace -ApplyPid --event syscall --max 10)
wperf_add_pwsh_test(cli_script_fixture tests/run_fixture_case.ps1
    -Wperf $<TARGET_FILE:wperf> -Fixture ${WPERF_GENERATED_ETW_DIR}/cpu-stack.etl -Command script -ApplyPid --limit 3)
wperf_add_pwsh_test(cli_script_csv tests/run_fixture_case.ps1
    -Wperf $<TARGET_FILE:wperf> -Fixture ${WPERF_GENERATED_ETW_DIR}/cpu-stack.etl -Command script -ApplyPid --limit 3 --fields timestamp,pid,comm,symbol --csv)
wperf_add_pwsh_test(archive_manifest_test scripts/archive_manifest_test.ps1
    -Wperf $<TARGET_FILE:wperf> -Fixture ${WPERF_GENERATED_ETW_DIR}/cpu-stack.etl -OutputDirectory ${WPERF_BINARY_DIR} -SchemaPath ${WPERF_SOURCE_DIR}/docs/schemas/archive.schema.json)
wperf_add_pwsh_test(replay_contract scripts/replay_contract.ps1
    -Wperf $<TARGET_FILE:wperf> -Fixture ${WPERF_GENERATED_ETW_DIR}/cpu-stack.etl -SymbolPath $<TARGET_FILE_DIR:wperf>)
wperf_add_pwsh_test(json_contract_test tests/json_contract_test.ps1
    -Wperf $<TARGET_FILE:wperf> -FixtureDirectory ${WPERF_GENERATED_ETW_DIR} -SchemaDirectory ${WPERF_SOURCE_DIR}/docs/schemas)
wperf_set_test_timeout(60 json_contract_test)
wperf_add_pwsh_test(trace_limit_test tests/trace_limit_test.ps1
    -Wperf $<TARGET_FILE:wperf> -Fixture ${WPERF_GENERATED_ETW_DIR}/sched.etl)
wperf_set_test_timeout(30 trace_limit_test)
wperf_add_pwsh_test(buildid_list_test tests/buildid_list_test.ps1
    -Wperf $<TARGET_FILE:wperf> -Fixture ${WPERF_GENERATED_ETW_DIR}/cpu-stack.etl)
wperf_set_test_timeout(30 buildid_list_test)
wperf_add_pwsh_test(priority_cli tests/priority_cli_test.ps1
    -Wperf $<TARGET_FILE:wperf> -FixtureDirectory ${WPERF_GENERATED_ETW_DIR}
    -OutputDirectory ${WPERF_BINARY_DIR}/priority-cli)
wperf_set_test_timeout(30 priority_cli)
wperf_add_pwsh_test(sched_output_test tests/sched_output_test.ps1
    -Wperf $<TARGET_FILE:wperf> -Fixture ${WPERF_GENERATED_ETW_DIR}/sched.etl)
wperf_set_test_timeout(30 sched_output_test)
wperf_add_pwsh_test(annotate_output_test tests/annotate_output_test.ps1
    -Wperf $<TARGET_FILE:wperf> -Fixture ${WPERF_GENERATED_ETW_DIR}/cpu-stack.etl -SymbolPath $<TARGET_FILE_DIR:wperf>)
wperf_set_test_timeout(30 annotate_output_test)
wperf_add_pwsh_test(diff_options_test tests/diff_options_test.ps1
    -Wperf $<TARGET_FILE:wperf> -Fixture ${WPERF_GENERATED_ETW_DIR}/cpu-stack.etl)
wperf_set_test_timeout(30 diff_options_test)

set_tests_properties(cli_report_conflicting_formats cli_annotate_conflicting_formats
    cli_off_cpu_missing_gate cli_off_cpu_strict_gate
    PROPERTIES WILL_FAIL TRUE)

set(WPERF_RUNTIME_ETW_ONLY_TESTS
    diagnostic_contract_test
    cli_report_io
    cli_report_io_json
    cli_folded_fixture
    cli_folded_output
    cli_report_flamegraph_cpu
    cli_report_flamegraph_offcpu
    cli_off_cpu_fixture
    cli_off_cpu_json
    cli_off_cpu_folded_output
    cli_off_cpu_missing_gate
    cli_off_cpu_strict_gate
    cli_report_conflicting_formats
    cli_annotate_conflicting_formats
    cli_sched_fixture
    cli_syscall_fixture
    cli_script_fixture
    cli_script_csv
    archive_manifest_test
    replay_contract
    json_contract_test
    trace_limit_test
    buildid_list_test
    priority_cli
    sched_output_test
    annotate_output_test
    diff_options_test
)

if(WPERF_RUNTIME_ETW_TESTS)
    wperf_add_pwsh_test(generate_etw_fixtures tests/generate_etw_fixtures.ps1
        -Wperf $<TARGET_FILE:wperf> -Workload $<TARGET_FILE:wperf_etw_fixture_workload>
        -OutputDirectory ${WPERF_GENERATED_ETW_DIR} -ElevateIfNeeded)
    add_test(NAME cleanup_etw_fixtures COMMAND ${CMAKE_COMMAND} -E remove_directory ${WPERF_GENERATED_ETW_DIR})
    set_tests_properties(generate_etw_fixtures PROPERTIES
        FIXTURES_SETUP wperf_generated_etw
        RUN_SERIAL TRUE
        SKIP_RETURN_CODE 77
        LABELS integration
        TIMEOUT 120)
    set_tests_properties(cleanup_etw_fixtures PROPERTIES
        FIXTURES_CLEANUP wperf_generated_etw
        RUN_SERIAL TRUE
        TIMEOUT 30)
    set_tests_properties(wperf_unit_tests wperf_regression_tests ${WPERF_RUNTIME_ETW_ONLY_TESTS}
                         PROPERTIES FIXTURES_REQUIRED wperf_generated_etw ENVIRONMENT "_NT_SYMBOL_PATH=")
else()
    set_tests_properties(${WPERF_RUNTIME_ETW_ONLY_TESTS} PROPERTIES DISABLED TRUE)
    set_tests_properties(wperf_unit_tests wperf_regression_tests
                         PROPERTIES ENVIRONMENT "_NT_SYMBOL_PATH=")
endif()

wperf_set_test_timeout(30 cli_report_flamegraph_cpu cli_report_flamegraph_offcpu)
