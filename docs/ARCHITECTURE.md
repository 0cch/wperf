# Architecture

wperf is a Windows ETW consumer with a deliberately small trust boundary. The
implementation is organized around one raw trace model, one replay path, and
multiple report/live consumers.

## Process model

```text
+-----------------------------+         authenticated pipe
| normal-integrity controller | <----------------------------+
| CLI, replay, symbols,       |                              |
| aggregation, JSON/CSV, UI   |                              |
+-----------------------------+                              |
                                                             v
                                              +-----------------------------+
                                              | hidden elevated worker      |
                                              | StartTrace/ProcessTrace     |
                                              | raw ETW windows, STOP       |
                                              +-----------------------------+
                                                             |
                                                             v
                                              +-----------------------------+
                                              | session guard               |
                                              | orphan cleanup + restore    |
                                              +-----------------------------+
```

The controller never needs to run elevated. The worker receives a nonce,
controller PID, session parameters, and a bounded output path. It starts or
stops ETW, enforces a stop deadline, and returns status/quality counters. The
guard owns cleanup if the worker disappears unexpectedly.

This boundary is central: arbitrary file parsing, DbgHelp, PDB lookup, and
unbounded aggregation stay outside the elevated process.

## Source layout

| Area | Files |
|---|---|
| CLI entry, catalog, and dispatch | `src/main.cpp`, `src/cli.cpp`, `src/commands.cpp`, `src/commands/` |
| ETW session and recovery | `src/etw.cpp`, `src/session_guard.cpp`, `src/state.cpp` |
| Elevated launch and IPC | `src/elevation.cpp`, `src/ipc.cpp` |
| Raw ETW decoding | `src/etw_decode.cpp`, `src/lbr.cpp` |
| ETL replay/model | `src/etl_reader.cpp`, `src/trace_model.cpp`, `src/trace_events.cpp` |
| Live capture | `src/live_top.cpp`, `src/live_data.cpp`, `src/commands/live_worker.cpp` |
| Analysis | `src/analysis.cpp`, `src/cpu_report.cpp`, `src/off_cpu_report.cpp` |
| Symbols and archives | `src/symbolizer.cpp`, `src/dbghelp_loader.cpp`, `src/archive.cpp` |
| Disassembly | `src/disassembler.cpp` |
| Rendering/output | `src/top_renderer.cpp`, `src/output.h` |
| Tests | `tests/`, `tests/data/`, `tests/workloads/` |

## CLI validation and documentation

`src/cli.cpp` owns the public command catalog: names, aliases, elevation,
options, examples, and compatibility notes. The runtime selects exactly one
command before invoking CLI11. Parsing only that command avoids ambiguity when
an option value equals another command name (for example, `trace -e sched`).

The same catalog renders text help and the generated Markdown command
reference. CLI11 validates option names and arity; wperf normalizes attached
short values and `--name=value` before dispatch. Separator forwarding is kept
verbatim so `-- child.exe --help` remains an argument to the child.

Two tests protect the boundary:

- `tests/cli_unit_tests.cpp` invokes the parser with a fake dispatcher and
  covers every public option and alias.
- `tests/cli_conformance_test.ps1` compares `help <command>` with
  `<command> --help` and fails when `docs/COMMAND_REFERENCE.md` is stale.

## Capture pipeline

1. The controller validates options and creates a run-state record.
2. `--elevate` launches one worker and authenticates a named pipe.
3. The worker configures the profile interval/PMC source, starts an ETW session,
   and enables the requested kernel providers.
4. The controller waits for READY before launching a child process.
5. The worker drains real-time buffers or writes a sequential ETL.
6. STOP is bounded; the worker reports lost buffers/events and restores global
   sampling state.
7. The guard stops orphaned sessions and restores configuration if the worker
   exits unexpectedly.

The system logger may independently report buffer loss. Successful process
exit alone is not proof of a complete trace; quality counters are part of the
result.

## Replay pipeline

`ReadEtwFile` uses `OpenTraceW` + `ProcessTrace` event callbacks. The replay
context performs four jobs:

1. decode kernel/classic events into a common model;
2. correlate process, thread, image, stack, and I/O identities;
3. resolve StackWalk and cached-stack references with lifetime-aware keys;
4. accumulate quality counters and finish derived analyses.

The same model drives reports, scripts, annotation, diff, and archive manifests.
Adding a new report should reuse this model instead of opening an ETL with a
second parser.

### Raw records versus derived data

The model keeps the raw value and its provenance where they differ:

- CPU sample `period` is nanoseconds;
- PMC sample `period` is a source-specific event count;
- CSwitch counter snapshots are estimated deltas;
- LBR entries are branch edges, not call-stack frames;
- context-register payloads are decoded only for validated AMD64 fields;
- sample weights are not fabricated from adjacent timestamps.

## Correlation and identity

- Process and thread lifetime records disambiguate PID/TID reuse.
- Images carry timestamp, checksum, size, path, and resolved path.
- Symbol aggregation keys include process instance, thread instance, module
  identity, and module-relative address where the report needs them.
- StackWalkRef/Def keys are lifetime-aware; stale references are rejected or
  counted as ambiguous.
- Wakeup stacks are stored separately from the waiting thread's stack.

When identity is insufficient, the report preserves `missing`, `ambiguous`, or
`unattributed` rather than assigning a plausible but unsafe owner.

## Off-CPU state

The offline scheduler model follows CSwitch and ReadyThread events:

```text
running --Wait--> blocked --ReadyThread--> ready --scheduled--> running
running --preemption--> ready --scheduled--> running
```

A blocked interval requires a switching-out `Wait`. The outgoing thread is
`related_tid`; `previous_state` belongs to that outgoing thread. Intervals that
cross the end of capture are marked truncated, not treated as confirmed
observations.

Reports expose confirmed and inferred duration separately. Folded output uses
nanoseconds as its weight.

## Live path

`LiveTopCapture` consumes events through a ProcessTrace thread. It owns bounded
deques for samples/events and bounded metadata indexes. A snapshot transfers a
window through the versioned `TOP2` IPC format to the controller.

The controller receives windows, drains the pipe while rendering, symbolizes
outside the receive lock, and reports unfinished/dropped windows. A stalled
renderer must not block STOP.

## Hardware features

### PMC

PMC interrupts use ETW profile sources. Source IDs and intervals are treated as
machine-global configuration. CSwitch snapshots use a separate ETW counter-list
configuration and produce estimated deltas.

### LBR

LBR is enabled through the current Windows LBR API when available and a legacy
ETW configuration sequence on platforms where the current API returns
not-supported. LBR events are decoded into from/to branch edges. Depth is
finite and hardware-dependent.

### Context registers

AMD64 context events use the validated Windows WMI `ContextRegistersAMD64`
field order: a QPC timestamp followed by RIP, RSP, and the general-purpose
registers in the documented order. The trace command exposes those fields only
for the validated architecture. ARM64 payload decoding is not claimed.

## Output and compatibility

Text output is for humans. JSON/CSV and exit codes are the automation contract.
JSON is serialized through nlohmann/json and the shared streaming writer, so
large trace/report output is not assembled as one string. Quality metadata is
intentionally verbose in JSON so callers can decide whether
a result is usable.

See [USAGE.md](USAGE.md) for command behavior and [COMPATIBILITY.md](COMPATIBILITY.md)
for platform and semantic limits.
