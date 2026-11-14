# Replay Fixtures

These checked-in x64 ETLs are compressed with
`xperf -merge -mergeonly -compress` and replay without administrator rights.

- `cpu-stack.etl` — CPU samples, direct stacks, and StackWalk correlation.
- `sched.etl` — CSwitch, ReadyThread, wait state/reason, and wakeup latency.
- `syscall.etl` — syscall enter/exit. The captured kernel provider lacks usable
  PID/TID data for some records, so the fixture intentionally does not fabricate
  durations.
- `io.etl` — FileIo/DiskIo initiation/completion correlation, status, and timing.

Each fixture has a matching `*.expected.json` contract used by tests. Keep
fixture bytes and expected contracts stable unless a change explicitly requires
new evidence. Do not add traces containing sensitive command lines, user names,
paths, or credentials without redaction and review.