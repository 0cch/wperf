# Replay fixtures

These native x64 traces are compressed with
`xperf -merge -mergeonly -compress` and can be replayed without administrator
rights. Counts are asserted by `wperf_unit_tests`; symbol names are not
asserted because PDB availability and published symbols vary by machine.

- `cpu-stack.etl`: CPU samples and StackWalk correlation.
- `sched.etl`: CSwitch, ReadyThread, wait reason/state and wakeup latency.
- `syscall.etl`: SysCallEnter/SysCallExit. The Windows kernel provider omits
  usable PID/TID in this capture, so syscall durations are intentionally not
  fabricated.
- `io.etl`: FileIo and DiskIo init/completion correlation, status and duration.

Each fixture has a matching `*.expected.json` file.
Stack fallback is limited to a preceding stack within 1 ms in the same thread
lifetime. The CPU fixture's former 44 cross-time fallbacks are now counted as
missing (1219 total); its 16104 exact matches and 496 ambiguous matches remain
unchanged. Fixture bytes and SHA256 have not changed.
