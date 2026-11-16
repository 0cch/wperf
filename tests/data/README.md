# Runtime ETW Replay Fixtures

No `.etl` binary is stored in this repository.

The test suite creates controlled ETW traces in the build directory through
`tests/generate_etw_fixtures.ps1` and `wperf_etw_fixture_workload.exe`.
Generation runs only on an elevated Windows test runner and produces:

- `cpu-stack.etl` - CPU samples, direct stacks, and stack correlation
- `sched.etl` - CSwitch, ReadyThread, wait state/reason, and wakeup latency
- `syscall.etl` - syscall enter/exit correlation
- `io.etl` - file/disk I/O initiation and completion correlation

The generated files live under the CMake build directory, are ignored by Git,
and are deleted by the `cleanup_etw_fixtures` CTest fixture cleanup test.
Do not add captured ETL files, dumps, user names, command lines, or machine
paths to the repository.
