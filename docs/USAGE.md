# Usage

## Capture

```powershell
wperf record [--elevate] [-F HZ] [-p PID] [--duration MS] [-o FILE]
             [--profile cpu|cpu-sched|sched|syscall|vm|io|kernel|all|pmu]
             [--pmc-source ID] [--lbr] [--context-registers]
             [--provider GUID] [--] [command...]
```

`record` captures a system ETW session by default. `--provider GUID` switches
to a private provider session. `--elevate` launches one hidden elevated worker
and keeps the interactive controller at the current integrity level.

## Reports

```powershell
wperf report -i FILE [--kind cpu|io] [--pid PID] [--tid TID]
             [--comm TEXT] [--dso TEXT] [--symbol TEXT]
             [--percent-limit N] [--sort FIELD]
             [--folded|--call-graph] [--csv|--json]
```

Quality information is written to stderr. Stack quality distinguishes exact,
approximate, ambiguous, missing and key-mismatched records.

## Live views

```powershell
wperf top [-p PID] [-e cpu|SOURCE] [--interval MS] [--iterations N]
          [--sort FIELD] [--percent-limit N] [--elevate]

wperf trace --live [-e sched|syscall|pagefault|io|lbr] [-p PID]
             [--duration MS] [--max N] [--json] [--elevate]
```

Bounded rings cap samples/events at 200000 entries. `ring_dropped` reports
overflow instead of silently growing memory.

## Offline trace and scheduling

```powershell
wperf script -i FILE [--pid PID] [--fields LIST] [--limit N] [--csv]
wperf trace -i FILE [-e sched|syscall|pagefault|io|lbr] [--pid PID] [--max N] [--json]
wperf sched -i FILE
wperf report -i FILE --kind io [--json]
```

## Symbols

```powershell
wperf report -i FILE --symbol-path DIR
wperf buildid-list -i FILE
wperf annotate -i FILE [--pid PID] [--limit N] [--disasm] [--json|--csv]
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
