# Usage

## Capture

```powershell
wperf record [--elevate] [-F HZ] [-e EVENT] [-g] [-p PID] [--duration MS] [-o FILE]
             [--profile cpu|cpu-sched|sched|syscall|vm|io|kernel|all|pmu]
             [--pmc-source ID] [--lbr] [--context-registers]
             [--provider GUID] [--] [command...]
```

`record` captures a system ETW session by default. `--provider GUID` switches
to a private provider session. `--elevate` launches one hidden elevated worker
and keeps the interactive controller at the current integrity level.

## Reports

```powershell
wperf report -i FILE [--kind cpu|io] [-p PID] [-t TID]
             [--comm TEXT] [--dso TEXT] [--symbol TEXT]
             [--percent-limit N] [-s FIELD]
             [-g [caller|callee]] [--folded|--collapsed]
             [--stdio|--csv|--json] [--quiet|--verbose]
```

A concise quality summary is written to stderr. Use `--verbose` for the complete
counter set or `--quiet` for clean machine-readable pipelines. Stack quality
distinguishes exact, approximate, ambiguous and missing records. ETW samples
with the `UINT32_MAX` sentinel PID are displayed as `[unattributed]`.

## Flame graphs

`--folded` is the current FlameGraph interface. It emits the standard collapsed
format, with the process name as the root frame:

```text
command;symbol;caller 123
```

The reference implementation is external and requires Perl:

```powershell
git clone --depth 1 https://github.com/brendangregg/FlameGraph.git artifacts\FlameGraph
```

Generate collapsed stacks and render them with an external FlameGraph tool:

```powershell
pwsh -NoProfile -File .\scripts\flamegraph.ps1 `
  -InputFile cpu.etl `
  -SymbolPath .\build\Release `
  -ProcessId 1234 `
  -PercentLimit 0.5 `
  -OutputFile flamegraph.svg
```

The helper writes UTF-8 without BOM and LF line endings, then invokes the
official `flamegraph.pl`. This avoids CRLF being included in the sample-count
field by Perl implementations on Windows. Set `FLAMEGRAPH_PL` or pass
`-FlameGraphScript` when the script is stored elsewhere.

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

## Offline trace and scheduling

```powershell
wperf script -i FILE [-p PID] [-F LIST] [--symbol-path DIR] [--limit N] [--csv]
wperf trace -i FILE [-e sched|syscall|pagefault|io|lbr] [--pid PID] [--max N] [--json]
wperf sched -i FILE
wperf report -i FILE --kind io [--json]
```

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

## Providers

```powershell
wperf list providers [--max N]
wperf record --elevate --provider GUID --duration 3000 -o provider.etl
```

Private-provider capture produces ETL for wperf, WPA and PerfView. Field-level
TDH rendering is outside the current CLI.
