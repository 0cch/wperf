# Usage

## Capture

```powershell
wperf record [--elevate] [-F HZ] [-e EVENT] [-g] [-p PID] [--duration MS] [-o FILE]
             [--profile cpu|cpu-sched|sched|syscall|vm|io|kernel|all|pmu]
             [--pmc-source ID] [--period N] [--lbr] [--context-registers]
             [--provider GUID] [--] [command...]
```

`record` captures a system ETW session by default. `--provider GUID` switches
to a private provider session. `--elevate` launches one hidden elevated worker
and keeps the interactive controller at the current integrity level.

The controller waits for capture READY before launching a target. CPU sampling
uses `-F HZ`; PMC sampling uses a source-specific event count with `--period N`.
Combining PMC selection with `-F` is rejected. A requested PMC/LBR capture that
produces no corresponding events returns unavailable (exit 5).

## Reports

```powershell
wperf report -i FILE [-o FILE] [--kind cpu|io] [-p PID] [-t TID]
             [--comm TEXT] [--dso TEXT] [--symbol TEXT]
             [--percent-limit N] [-s FIELD]
             [-g [caller|callee]] [--folded|--collapsed] [--off-cpu]
             [--off-cpu-include-inferred] [--include-children] [--cpu N]
             [--time-start MS] [--time-end MS] [--scope all|user|kernel]
             [--stdio|--csv|--json] [--quiet|--verbose]
             [--exact-stacks] [--metadata] [--strict-quality]
             [--max-lost-events N] [--max-missing-stacks N]
```

A concise quality summary is written to stderr. Use `--verbose` for the complete
counter set or `--quiet` for clean machine-readable pipelines. Stack quality
distinguishes exact, approximate, ambiguous and missing records. ETW samples
with the `UINT32_MAX` sentinel PID are displayed as `[unattributed]`.

`--json --metadata` wraps rows with machine-readable quality information.
`--strict-quality` fails on lost, dropped or malformed data; missing stacks
alone do not cause failure. `--exact-stacks` excludes uncertain stacks.
`--max-lost-events` and `--max-missing-stacks` apply to every report form,
including off-CPU. For off-CPU, missing stacks are counted as affected
intervals. Malformed, lost, or otherwise incomplete trace data returns exit 6
after writing the selected report output.

## Flame graphs

`--folded` is the current FlameGraph interface. It emits the standard collapsed
format, with the process name as the root frame:

```text
command;module!caller;module!symbol 123
```

The reference implementation is external and requires Perl:

```powershell
git clone --depth 1 https://github.com/brendangregg/FlameGraph.git artifacts\FlameGraph
```

Generate collapsed stacks and render them with an external FlameGraph tool:

```powershell
wperf report -i cpu.etl `
  --symbol-path .\build\Release `
  --pid 1234 `
  --folded --percent-limit 0.5 --quiet `
  -o stacks.folded

perl flamegraph.pl stacks.folded > flamegraph.svg
```

`report -o` writes UTF-8 without BOM and LF line endings directly. This avoids
CRLF being included in the sample-count field by Perl implementations on
Windows, without a wrapper script.

`--collapsed` is an alias for `--folded`. `--percent-limit` applies to folded
stacks as well as table, CSV and JSON reports. Use `--pid` to isolate a process
before rendering a multi-process trace. Native SVG/HTML output is intentionally
not part of this stage.

## Off-CPU analysis

Schedule-stack capture is requested automatically for `record --profile sched`,
`cpu-sched` and `all`. `report --off-cpu` consumes the correlated blocked/ready
intervals:

```powershell
wperf report -i sched.etl --off-cpu --symbol-path .\build\Release
wperf report -i sched.etl --off-cpu --json
wperf report -i sched.etl --off-cpu --folded --quiet -o offcpu.folded
wperf report -i sched.etl --off-cpu --metadata --json
wperf report -i sched.etl --off-cpu --off-cpu-include-inferred --csv
```

Folded counts are wait duration in nanoseconds. Render with:

```powershell
flamegraph.pl --countname ns offcpu.folded > offcpu.svg
```

Blocked intervals require a context switch from `Wait`. Normal preemption and
ready-but-not-running time are reported separately. ETW `CSwitch` uses
`tid=incoming` and `related_tid=outgoing`; `previous_state` belongs to the
outgoing thread. Wakeup stacks are retained separately and are not assigned as
the blocked thread's stack.

JSON/CSV rows separate wakeup sources by PID/TID, process/thread lifetime and
stack identity. `wakeup_pid`, `wakeup_tid`, `wakeup_process_start_ns` and
`wakeup_thread_start_ns` describe that source; JSON `wakeup_stack_frames` and
CSV `wakeup_stack` resolve its frames in the waking process. Folded output
remains weighted by the waiting thread's stack, so a FlameGraph can combine
waiting paths across wakeup sources without mixing their call stacks.

Intervals closed only by the end of capture are marked `boundary=truncated` and
the confirmed blocked portion remains available when only the ready tail is
unfinished. Truncated intervals
are excluded from exact totals by default. Use `--off-cpu-include-inferred` to
include them; folded output then appends `[truncated-boundary]`. JSON and CSV
always expose `confirmed_duration_ns`, `inferred_duration_ns`, and boundary
status. Aggregation keeps process instances, thread instances, module identity,
and module-relative addresses separate, avoiding PID/TID reuse merges.

`--time-start` and `--time-end` are capture-relative milliseconds and crop
intervals crossing the selected bounds. `--include-children` and
`--scope user|kernel` are supported by the off-CPU path.

## Command help

```powershell
wperf help
wperf help report
wperf report --help
```

`-g` on `record` is accepted for perf compatibility; system ETW stack capture is
enabled by default and can be disabled with `--no-call-graph`. `--stdio` is
accepted by `report` for scripts that pass perf-style text output options.

## Live views

```powershell
wperf top [-p PID] [-e cpu|SOURCE] [--interval MS] [--iterations N]
          [--sort FIELD] [--symbol-path DIR] [--archive DIR]
          [--percent-limit N] [--elevate]

wperf trace --live [-e sched|syscall|pagefault|io|lbr] [-p PID]
             [--duration MS] [--max N] [--json] [--elevate]
```

`top --symbol-path DIR` supplies additional PDB locations to the elevated live
worker. Its summary reports `unattributed`, `unknown_events` and
`ring_dropped` so incomplete attribution is visible.

Bounded rings cap samples/events at 200000 entries. `ring_dropped` reports
overflow instead of silently growing memory.

Thread/image metadata and symbol caches also have limits; ended metadata is
retained briefly for delayed events. ETW loss and correlation eviction are
reported separately. Pending correlations expire after 60 seconds.

## Offline trace and scheduling

```powershell
wperf script -i FILE [-p PID] [-F LIST] [--symbol-path DIR] [--limit N] [--csv]
wperf trace -i FILE [-e sched|syscall|pagefault|io|lbr|context] [--pid PID] [--max N] [--json]
wperf sched -i FILE
wperf report -i FILE --kind io [--json]
```

The script fields `source`, `period` and `stack_quality` expose sample provenance.
CPU period is in nanoseconds; PMC period is a source-specific count; zero means
unavailable. Diff uses CPU sample periods and falls back to the trace-level
interval when a sample period is unknown; late metadata handling remains a
follow-up. It does not assign sampling gaps or PMC counts as execution time.

`trace -e context` emits decoded AMD64 context-register events from an offline ETL. The JSON object includes RIP, RSP, and RAX/RBX/RCX/RDX/RSI/RDI/RBP/R8-R15; ARM64 payloads are not decoded.

`script` text is wperf-specific and is not intended as input to
`stackcollapse-perf.pl`. Use `report --folded` for FlameGraph.

## Symbols

```powershell
wperf report -i FILE --symbol-path DIR
wperf buildid-list -i FILE
wperf annotate -i FILE [--pid PID] [--limit N] [--disasm] [--stdio|--json|--csv]
```

`annotate --disasm` shows instruction bytes and Zydis Intel-syntax disassembly.
With `-DWPERF_ENABLE_DISASM=OFF`, it falls back to address heat with a warning.

## Diff and archive

```powershell
wperf diff -1 BEFORE.etl -2 AFTER.etl [--pid PID] [--dso TEXT]
           [--calltree] [--json|--csv]

wperf archive -i FILE -o DIRECTORY [-p PID] [--symbol-path DIR]
              [--max-size-mb N]
```

Archive writes `manifest.json`, `manifest.csv` and `manifest.txt` with PE/PDB
identity, SHA256, copy status and size-limit decisions.

Referenced local binaries must match the trace identity. Binaries are stored
under `modules/<SHA256>` and matching PDBs in a `symbols` symbol-store layout.
Manifests contain relative paths; the ETL itself is not copied.

## Providers

```powershell
wperf list providers [--max N]
wperf record --elevate --provider GUID --duration 3000 -o provider.etl
```

Private-provider capture produces ETL for wperf, WPA and PerfView. Field-level
TDH rendering is outside the current CLI.
