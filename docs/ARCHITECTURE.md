# Architecture

## Components

- `etw.*`: ETW session lifecycle, profile sources, PMC ownership, providers and
  context-register capability configuration.
- `etl_reader.*`: replay, classic event decoding, sample/stack correlation and
  module image lifecycle.
- `etw_decode.*`: shared CPU/PMC sample, period and processor-index decoding.
- `trace_events.*`: scheduler/syscall/I/O correlation and quality accounting.
  One bounded incremental state machine serves replay and live snapshots.
- `lbr.*`: last-branch payload decoding.
- `symbolizer.*`: PE/RSDS identity validation, DbgHelp, synthetic module bases,
  source line and in-memory symbol cache.
- `live_top.*`: real-time ETW consumer and bounded sample/event rings.
- `live_data.*`: versioned, size-bounded structured IPC windows for low-privilege controller-side analysis.
- `off_cpu_report.*`: blocked/ready interval aggregation and folded report output.
- `commands/`: CLI controllers, live worker protocol, reports and analysis.
- `zydis`: optional static disassembly engine.

## Replay pipeline

1. Open ETL with raw timestamps.
2. Reconstruct process, thread and image lifetimes.
3. Decode samples, scheduler, syscall, page-fault, I/O and LBR records.
4. Validate payload lengths before reads.
5. Correlate samples to stacks using raw QPC + TID + CPU; use leaf IP only to
   disambiguate multiple stacks at the same key.
6. Correlate syscall/I/O/ready transitions and account for orphans.
7. Symbolize module-relative addresses and aggregate the requested report.

## Live pipeline

- Elevated hidden worker owns one real-time ETW session.
- Callback decodes payloads into bounded sample/event rings and lifecycle metadata.
- Snapshot exchanges ring buffers under a short lock, then correlates outside it.
- Controller and worker communicate through a nonce-authenticated named pipe.
- The worker validates the named-pipe server PID against the expected controller.
- DbgHelp and formatting run outside the ETW callback and outside the controller receive loop.
- A bounded asynchronous renderer consumes completed windows, so symbol lookup and output cannot block STOP processing.
- Ctrl-C sends STOP, keeps draining control messages, waits for worker exit and cleans stale sessions if needed.
- The elevated worker sends raw trace windows and metadata; the normal-integrity
  controller owns DbgHelp, PDB lookup, archive resolution, symbol caches,
  aggregation, and formatting.
- Live IPC uses a bounded, versioned line protocol. String and U64 payloads are
  truncated below the pipe receive limit, and truncation increments explicit
  metadata quality counters. The worker checks the control pipe between
  messages, and client pipe handles use overlapped I/O with bounded write
  deadlines. STOP remains responsive and a stalled reader cannot block the
  worker indefinitely.
- Live and record workers have a session guard for abnormal exit and conditional
  sampling-configuration recovery.

## Data integrity

- Exact stack matches are separated from approximate matches.
- Unknown, lost, ambiguous, key-mismatched, metadata-dropped and ring-dropped data is counted.
- Scheduler-event stacks are correlated separately from sample stacks.
- Off-CPU blocked intervals require a switch from Wait; ready intervals and
  preemption are represented separately. End-of-capture closures retain a
  truncated boundary flag and are excluded from exact totals by default.
- Off-CPU grouping uses process/thread instance starts plus module identity and
  RVA, and resolves stacks at the original interval or wakeup timestamp.
- PE timestamp/checksum and RSDS PDB GUID/age are validated before symbols are
  trusted.
- PMC values are labeled sample-derived and are not presented as exact totals.
- DbgHelp API calls are globally serialized; each Symbolizer owns a unique
  session key. Indexed address lookup precedes the bounded symbol cache.
- Unsupported LBR/context-register capabilities are never synthesized.

## Dependency boundary

- No custom driver.
- No MSR access.
- No kernel modification.
- No WPA/WPR runtime requirement.
- Zydis is the only optional third-party runtime feature, statically linked.
