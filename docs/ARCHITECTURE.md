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
| Interrupt/memory/driver diagnostics | `src/diagnostic_model.h`, `src/diagnostic_decode.cpp`, `src/diagnostic_report.cpp`, `src/commands/diagnostics.cpp` |
| Symbols and archives | `src/symbolizer.cpp`, `src/dbghelp_loader.cpp`, `src/archive.cpp` |
| Disassembly | `src/disassembler.cpp` |
| Rendering/output | `src/top_renderer.cpp`, `src/output.h` |
| Tests | `tests/`, `tests/workloads/`, runtime-generated ETW fixtures under the build directory |

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
   Capture metadata provider enablement also waits for completion before writing
   period/source metadata; asynchronous enablement could otherwise discard the
   first metadata event while EventWrite still returns success.
5. The worker drains real-time buffers or writes a sequential ETL.
6. STOP is bounded; the worker reports lost buffers/events and restores global
   sampling state.
7. The guard stops orphaned sessions and restores configuration if the worker
   exits unexpectedly. The worker writes recovery fields to a separate atomic
   `<state>.restore` checkpoint, validated by correlation ID and worker PID.
   Controller status writes cannot overwrite that checkpoint. Legacy state
   fields remain a fallback for older runs.

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

- Process and thread lifetime records disambiguate PID/TID reuse. Start-rundown
  describes an initial baseline (start/load 0) rather than a birth at the delayed
  metadata delivery time; actual start/load events keep their timestamps. End
  rundown reports continued presence and never acts as exit/unload.
- Images carry timestamp, checksum, size, path, and resolved path.
- Modern image payload PID 0 is a valid global owner, including kernel rundown;
  it is not replaced with the emitting header PID. Kernel address lookup shares
  PID 0/4 images across target processes while respecting load/unload lifetimes.
  Version-0 payloads have no PID and use the header; unavailable payload PID
  `UINT32_MAX` retains the header fallback.
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
observations. Inferred upper bounds also stop at thread/process termination.
Quality is finalized after all derived interval counters are populated.

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

Diagnostic reports use independent interrupt, allocation, and driver records.
The shared reader can retain only the requested domain plus lifecycle metadata
and required allocation stacks; irrelevant high-rate events do not consume its
managed replay budget. Heap uses a native ETW heap-session PID extension rather
than persistent IFEO configuration. Pool enables only the pool group mask and a tag filter, avoiding unrelated
high-rate memory/page-list events. Driver capture adds CSwitch for explicitly
labeled entry-context reconstruction.
Domain analysis preserves global interrupt/pool ownership and correlates memory
lifetimes before allocation-cohort selection. See [DIAGNOSTICS.md](DIAGNOSTICS.md).

Text output is for humans. JSON/CSV and exit codes are the automation contract.
JSON is serialized through nlohmann/json and the shared streaming writer, so
large trace/report output is not assembled as one string. Quality metadata is
intentionally verbose in JSON so callers can decide whether
a result is usable.

The Agent surface is deliberately thin. `capabilities --json` serializes the
same command catalog used by help and runtime validation. The top-level
`--json-errors` adapter suppresses text errors, maps the process exit code to a
stable error code/action pair, and emits one JSON object on stderr. `record
--json` emits a success envelope but does not replay the ETL solely to populate
quality; report metadata remains the source of quality gates. See
[AGENT.md](AGENT.md) for the workflow contract.

See [USAGE.md](USAGE.md) for command behavior and [COMPATIBILITY.md](COMPATIBILITY.md)
for platform and semantic limits.

CPU aggregation tracks counts and periods separately and rejects mixed units.
Streaming output emits final quality after rows. Build identity remains strict
for symbol loading/cache; cross-build diff matching uses a separate symbol key.
Module-list file metadata and optional hashes are cached within each invocation.

Flame graphs are generated from the same folded-stack representation used by
CPU and off-CPU reports. `flamegraph.cpp` owns folded parsing and an indexed tree
arena, then serializes the tree iteratively so accepted deep stacks do not consume
the native call stack. `flamegraph_html.cpp` owns the self-contained HTML
presentation, and `flamegraph_assets.h` contains the inline CSS/JavaScript
assets. The writer embeds exact decimal weight strings for JavaScript display and
copy operations while using numbers only for geometry; range-selection derived
weights are explicitly marked approximate. The writer embeds quality counters,
filters, and the selected weight label; it does not call a symbolizer or download
assets at render time. The standalone `flamegraph` command applies the same
renderer to an existing folded file.

Symbol diagnostics are copied out of the controller-side `Symbolizer` after
resolution. They separate in-memory cache hits, persistent `.symcache` hits, and
DbgHelp work, and retain module rejection reasons plus DbgHelp/PDB paths. Text
verbose output is bounded; JSON metadata retains the structured module detail.
