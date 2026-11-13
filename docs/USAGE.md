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
             [-g [caller|callee]] [--folded|--collapsed]
             [--stdio|--csv|--json] [--quiet|--verbose]
             [--exact-stacks] [--metadata] [--strict-quality]
```

A concise quality summary is written to stderr. Use `--verbose` for the complete
counter set or `--quiet` for clean machine-readable pipelines. Stack quality
distinguishes exact, approximate, ambiguous and missing records. ETW samples
with the `UINT32_MAX` sentinel PID are displayed as `[unattributed]`.

`--json --metadata` wraps rows with machine-readable quality information.
`--strict-quality` fails on lost, dropped or malformed data; missing stacks
alone do not cause failure. `--exact-stacks` excludes uncertain stacks.

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
          [--sort FIELD] [--symbol-path DIR] [--percent-limit N] [--elevate]

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
wperf trace -i FILE [-e sched|syscall|pagefault|io|lbr] [--pid PID] [--max N] [--json]
wperf sched -i FILE
wperf report -i FILE --kind io [--json]
```

The script fields `source`, `period` and `stack_quality` expose sample provenance.
CPU period is in nanoseconds; PMC period is a source-specific count; zero means
unavailable. Diff uses CPU sample periods and falls back to the trace-level
interval when a sample period is unknown; late metadata handling remains a
follow-up. It does not assign sampling gaps or PMC counts as execution time.

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
