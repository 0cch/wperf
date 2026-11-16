# Usage

This guide describes the stable workflows exposed by the current wperf CLI.
For the exhaustive option list of the installed build, use `wperf help`,
`wperf help <command>`, `wperf <command> --help`, or the generated
[command reference](COMMAND_REFERENCE.md).

## Conventions

- `wperf` means `wperf.exe` from the build or release directory.
- Input traces are ETW `.etl` files.
- Paths in examples are relative to the repository root.
- System capture requires an elevated worker. `--elevate` starts that worker
  without raising the interactive controller.
- JSON and CSV are intended for scripts. Diagnostics go to stderr unless
  `--quiet` is used.
- Option values may be separate (`-F 99`) or attached (`-F99`). Long forms
  may use `--name=value`.
- Use `--` to stop wperf option parsing and pass the remaining arguments to a
  launched command.
- `wperf help <command>` and `wperf <command> --help` render the same text.
  `wperf help --format markdown` emits the generated command reference.

## Choose a workflow

| Goal | Start here |
|---|---|
| Find slow CPU functions | `record` → `report` |
| See live hot functions | `top` |
| Diagnose stalls / waiting | `record --profile sched` → `report --off-cpu` |
| Inspect scheduling or syscalls | `trace` |
| Compare two builds | `diff` |
| Inspect instruction heat | `annotate` |
| Reproduce symbols later | `archive` |
| Inspect hardware counters / LBR | `record --pmc-source`, `--lbr`, `--context-registers` |

## First successful profile

```powershell
# Use the built-in CPU workload to verify the install.
wperf record --elevate --duration 5000 -o cpu.etl -- .\build\Release\wperf_cpu_burn.exe 5000 2

# Print the hottest functions and stacks.
wperf report -i cpu.etl --symbol-path .\build\Release --call-graph
```

Check the exit code, lost-event counters, resolved modules, and missing-stack
counters first. Use `--verbose` for the complete quality metadata. If symbols
are missing, pass the matching PDB directory with `--symbol-path`. If capture
fails to start, run `wperf doctor`.

## Capture with `record`

```text
wperf record [options] [--] [command [arguments...]]
```

Common examples:

```powershell
# CPU samples for the whole system.
wperf record --elevate --duration 5000 -o cpu.etl

# CPU samples at 999 Hz.
wperf record --elevate -F 999 --duration 5000 -o cpu-999.etl

# Attach to an existing process.
wperf record --elevate -p 1234 --duration 5000 -o attached.etl

# Launch and profile a child process.
wperf record --elevate --duration 10000 -o child.etl `
  -- .\app.exe --input data.txt
```

`record` waits for the elevated side to report READY before launching a child.
When the child exits, capture stops automatically.

Without `--duration`, system-wide capture runs until Ctrl+C; when a target command
is supplied, it stops when that root process exits. `-p` observes an existing process
uses system-wide ETW capture plus analysis filtering for system profiles.
`--profile heap -p PID` instead enables native heap tracing for that existing
process; allocations before attachment are outside its baseline.

### Profiles and events

`--profile` enables common event groups:

| Profile | Purpose |
|---|---|
| `cpu` | Timer/CPU sampling, default |
| `cpu-sched` | CPU samples plus scheduler events and stacks |
| `sched` | Scheduler events, CSwitch/ReadyThread stacks |
| `syscall` | Syscall enter/exit events |
| `vm` | Page-fault events |
| `io` | File and disk I/O events |
| `kernel` | Kernel event set |
| `all` | Broad system trace set |
| `pmu` | ETW PMC interrupt sampling |
| `dpc`, `isr` | Dedicated DPC or ISR execution events, without CPU sampling |
| `heap` | Native heap alloc/free/realloc for an existing `-p PID` |
| `pool` | Pool alloc/free events and allocation stacks; requires `--pool-tag TAG` |
| `driver` | Driver call/return boundaries plus I/O events |

`-e NAME|ID` accepts a CPU profile name, an ETW profile-source name, or a
numeric source ID. Use `wperf list` to inspect available profile sources.

### Stacks and stack cache

System ETW call stacks are enabled by default. Disable them with
`--no-call-graph`; `-g` is accepted for perf-style compatibility.

`--stack-cache` requests modern StackWalkDef/Ref records when the Windows
version and trace provider support them. Offline replay can consume both direct
StackWalk events and cached-stack references.

### Sampling frequency and periods

CPU sampling uses `-F HZ` (1-100000, default 1000). PMC sampling uses a
source-specific event count with `--period N`. Do not combine `-F` with PMC
selection.

```powershell
wperf record --elevate -e TotalCycles --period 1000000 `
  --duration 3000 -o pmc.etl
```

A requested PMC or LBR capture that produces no corresponding records exits
with an unavailable-result status instead of silently producing an empty profile.

### CSwitch PMC snapshots

`--pmc-counters IDS` samples the listed counters on context switches rather than
creating PMC interrupts:

```powershell
wperf record --elevate --pmc-counters 19,26 --duration 3000 `
  -o snapshots.etl
wperf report -i snapshots.etl --kind pmc --json --metadata
```

These values are per-CPU/old-thread deltas derived from event snapshots. They
are always marked `exact:false` and `event-snapshot-derived`; they are not exact
hardware totals.
PMC interrupt sampling (`-e SOURCE`/`--pmc-source`) and `--pmc-counters` snapshots
must use separate captures; combining them is an argument error.

### Intel LBR

`--lbr` requests Last Branch Record capture on supported Intel hardware.
wperf first tries the current Windows API and falls back to the legacy ETW LBR
configuration sequence where required.

```powershell
wperf record --elevate -F 99 --duration 3000 --lbr --no-call-graph `
  -o lbr.etl
wperf trace -i lbr.etl -e lbr --json
```

LBR usefulness depends on the CPU, Windows build, privilege, and available LBR
depth. It is not Intel Processor Trace.

### AMD64 context registers

`--context-registers` requests context-register events for configured kernel
events. Offline trace output decodes the validated AMD64 payload fields:

```powershell
wperf record --elevate --context-registers --duration 2000 `
  --no-call-graph -o context.etl
wperf trace -i context.etl -e context --json
```

The JSON `context_registers` object contains RIP, RSP, and the validated GPR
order. ARM64 context payload decoding is not implemented.

### Private ETW providers

```powershell
wperf record --elevate --provider {PROVIDER-GUID} --duration 3000 `
  -o provider.etl
wperf list providers --max 20
wperf list --json
wperf doctor --json
```

Private-provider capture produces a normal ETL for wperf, WPA, PerfView, or
other ETW consumers. TDH rendering of arbitrary provider payloads is outside
the current CLI.

## Reports

```text
wperf report -i FILE [options]
```

| Need | Flag |
|---|---|
| Human table | default |
| Call tree | `--call-graph callee` or `caller` |
| FlameGraph input | `--folded` / `--collapsed` |
| Scripts | `--json` or `--csv` |
| Quality object | `--json --metadata` |
| Blocked/ready wait time | `--off-cpu` |
| I/O summary | `--kind io` |
| PMC snapshot deltas | `--kind pmc --json --metadata` |
| Interrupt routine time | `--kind dpc`, `isr`, or combined `interrupt` |
| Native allocation lifetimes | `--kind heap` or `pool` |
| Observable driver calls | `--kind driver` |

These diagnostic kinds support text/JSON/CSV and file output. See
[DIAGNOSTICS.md](DIAGNOSTICS.md) for capture examples, supported selectors,
quality fields, and the distinction between inclusive time, observed live
allocations, and measurable driver call spans.

### Flat CPU profile

```powershell
wperf report -i cpu.etl --symbol-path .\build\Release
wperf report -i cpu.etl --sort symbol --percent-limit 1
wperf report -i cpu.etl --json --metadata
```

Useful selectors include `--pid`, `--tid`, `--comm`, `--dso`, `--symbol`,
`--cpu`, `--time-start`, `--time-end`, `--scope`, and `--include-children`.
`--exact-stacks` excludes approximate or ambiguous stack correlations.

`--weight period` is the default: tables and trees use the sum of known periods
(CPU nanoseconds or PMC event counts). `--weight samples` uses counts. If any
selected period is unknown, the whole selected report falls back to counts;
JSON exposes `overhead_basis` and `normalization_fallback`. Mixed sample domains
are rejected; select one with `--source ID`. Sorting defaults to weight in period
mode and samples in count mode; `--sort weight` is also accepted.

I/O and PMC snapshot reports support PID/TID/comm/children/CPU/time selection,
`--max-lost-events` and `--strict-quality`. PMC snapshot time windows include only
deltas whose two endpoints are within the window; filtering retains counter
baselines. PMC snapshots support CSV and `--source ID`. I/O supports text/JSON.
Symbol/stack/scope/weight/sort/percent options are rejected for these kinds;
missing-stack gates apply to CPU/off-CPU or heap/pool allocation stacks, not
I/O/PMC snapshots. I/O init/complete/orphan diagnostics
and report quality gates describe the full capture; I/O event/file metrics describe
the selection. An empty I/O/PMC selection exits 5.

`--stream` omits stack assessment (`stacks_loaded=false`,
`stack_assessment=not-loaded`); omitted stacks are not counted as missing.
Streaming JSON emits rows first and final quality after the second pass.
`--max-missing-stacks` and `--exact-stacks` cannot be used with `--stream`.

### Call tree and folded stacks

```powershell
wperf report -i cpu.etl --symbol-path .\build\Release --call-graph callee
wperf report -i cpu.etl --symbol-path .\build\Release --call-graph caller
wperf report -i cpu.etl --symbol-path .\build\Release `
  --weight samples --folded --quiet -o stacks.folded
```

Folded output is the standard collapsed-stack format, rooted at the process
name. Weights are CPU nanosecond periods (PMC event counts for PMC samples), or
sample counts with `--weight samples` or unknown-period fallback. Off-CPU weights
remain nanoseconds. `-o` writes UTF-8 without BOM and LF line endings.

Render with an external FlameGraph implementation:
The example above explicitly selects sample counts for the renderer's default
sample labels. For confirmed period weights, use `--countname nanoseconds` for
CPU or `--countname events` for PMC, and check for unknown-period fallback.

```powershell
perl flamegraph.pl stacks.folded > flamegraph.svg
```

### JSON, CSV, and quality gates

`--json` and `--csv` are mutually exclusive. `--metadata` adds quality metadata
to JSON output. Use `--verbose` for every quality counter or `--quiet` for clean
pipelines.

Quality fields answer different questions:

| Field | Meaning |
|---|---|
| `exact_stacks` | stack matched the same sample time/thread/CPU |
| `approximate_stacks` | stack matched a bounded nearby event |
| `missing_stacks` | no safe stack could be assigned |
| `lost_events` / `lost_buffers` | ETW itself lost data |
| `ring_dropped` | wperf live capture dropped records |
| `decode_errors` | a record did not match its expected schema |
| `incomplete` | the result must not be treated as a clean total |
| `exact:false` | the value is estimated or derived |

`--strict-quality` fails on malformed, lost, or incomplete data.
`--max-lost-events N` and `--max-missing-stacks N` apply configurable gates.
Unresolved stack gaps are visible rather than silently assigned to another
thread or time.

### Off-CPU analysis

Scheduler-stack capture is requested by `record --profile sched`,
`--profile cpu-sched`, and `--profile all`.

```powershell
wperf record --elevate --profile sched --duration 5000 -o sched.etl
wperf report -i sched.etl --off-cpu --symbol-path .\build\Release
wperf report -i sched.etl --off-cpu --json --metadata
wperf report -i sched.etl --off-cpu --folded --quiet -o offcpu.folded
```

The off-CPU report separates:

- blocked intervals from `Wait`;
- preempted/ready-but-not-running intervals;
- wakeup source identity;
- the waiting thread's stack and the waking thread's stack.

Intervals closed only by the end of capture are marked as truncated. They are
excluded from exact totals by default. Use `--off-cpu-include-inferred` to
include them. Folded off-CPU counts are wait duration in nanoseconds.

## Live sampling: `top`

```powershell
wperf top --elevate --interval 1000
wperf top --elevate -p 1234 --iterations 10
wperf top --elevate -e TotalCycles --symbol-path .\symbols
```

A live window reports `unattributed`, `unknown_events`, and `ring_dropped`.
PDB parsing and symbolization run in the normal-permission controller, not in
the elevated worker.

## Live tracing: `trace --live`

```powershell
wperf trace --live --elevate -e sched --duration 5000
wperf trace --live --elevate -e syscall --json --max 100
wperf trace --live --elevate -e lbr --duration 2000
```

Live tracing is supported for scheduler, syscall, page-fault, I/O, and LBR
events. Context-register decoding is currently available for offline ETLs:
`trace -i FILE -e context`.

## Offline event inspection

```powershell
wperf trace -i trace.etl -e sched --max 20
wperf trace -i trace.etl -e syscall --json
wperf trace -i trace.etl -e pagefault
wperf trace -i trace.etl -e io
wperf trace -i trace.etl -e lbr --max 100
wperf trace -i trace.etl -e context --json
```

Event filters:

| Filter | Records |
|---|---|
| `sched` | CSwitch and ReadyThread |
| `syscall` | Syscall enter/exit |
| `pagefault` | Page-fault events |
| `io` | File and disk I/O |
| `lbr` | LBR branch records |
| `context` | AMD64 context-register records |

`--max` defaults to `0`, meaning no output limit. A finite limit emits a warning
when truncation occurs. `--json --metadata` wraps events with `quality` and a
`truncated` boolean.

## Sample scripting

`script` exports one line per sample.

```powershell
wperf script -i cpu.etl --limit 20
wperf script -i cpu.etl `
  -F timestamp,cpu,pid,tid,ip,comm,dso,symbol,source,period,stack_quality `
  --csv
```

The text format is wperf-specific and is not intended as input to
`stackcollapse-perf.pl`. Use `report --folded` for FlameGraph input.

`script -o FILE` writes the selected text/CSV to UTF-8 without BOM and LF line
endings, with no duplicate stdout output. Parent directories are created and
write failures exit 9.

## Scheduler summary and `stat`

```powershell
wperf sched -i sched.etl
wperf stat -p 1234 --duration 2000
wperf stat --per-thread -p 1234
wperf stat --pmu -e TotalCycles -- .\app.exe
```

`stat` reports observed process/thread counters. PMC mode is ETW-derived and
should not be treated as a hardware counter programming API.

Use `--json` or `--csv` for automation and `--interval MS` for repeated process
statistics. PMC mode reports `source=etw-pmc-sampling`, `exact=false`, and the
configured sampling period (`--period`, default 1000000); it is not an exact
counter total.

`--per-thread` works in text, JSON and CSV for Windows API mode. Counters are
deltas over the observation, not thread lifetime totals. Threads are discovered
every 50 ms; observed exited threads retain handles and distinct creation times.
Threads created and exited entirely between polls can be missed. JSON adds
`threads`, `threads_complete` (whether observed counters could be accessed), and
`thread_counter_scope=observation-delta`. This flag does not claim every thread
was observed. CSV adds process/thread records with the header:

```text
row_type,elapsed_ms,pid,tid,thread_start_100ns,user_ms,sys_ms,cycles,page_faults,io_read_ops,io_read_bytes,io_write_ops,io_write_bytes,target_exit_code
```

`thread_start_100ns` is the absolute Windows FILETIME creation value; absent
thread/process-only CSV fields are empty. Without `--per-thread`, the existing
process CSV header is unchanged. PMC mode rejects `--per-thread` and `--interval`.

## Symbols

```powershell
wperf report -i cpu.etl --symbol-path .\build\Release
wperf buildid-list -i cpu.etl
wperf buildid-list -i cpu.etl --json
wperf buildid-list -i cpu.etl --csv
wperf buildid-list -i cpu.etl --hash --json
```

`buildid-list` preserves ETL `pe_timestamp`, `pe_checksum`, mapping size/PID/base
and `recorded_identity`. `local_identity` contains the current file's PE/PDB
metadata; top-level machine/PDB fields also describe that local file. Status is
`matching`, `mismatched` or `missing`. A valid PE file alone is not a match.
SHA256 is optional (`--hash`), cached once per normalized local path, and always
describes the local file (`module_sha256_source=local-file`). Without `--hash`,
`module_sha256` is empty and `hash_status=not-requested`; requested hashes are
`computed` or `unavailable`. All mapping rows are retained. CSV appends
`local_pe_timestamp,local_pe_checksum,local_size_of_image,hash_status`.

Symbol resolution uses DbgHelp, local PDBs, optional symbol-server settings,
and the project's identity-aware `.symcache`. Module timestamp/checksum/PDB
identity mismatches are rejected rather than merged.

For public Microsoft symbols, configure a local cache and server in
`_NT_SYMBOL_PATH`:

```powershell
$env:_NT_SYMBOL_PATH = 'srv*D:\Symbols*https://msdl.microsoft.com/download/symbols'
wperf report -i cpu.etl --call-graph --verbose
```

wperf delay-loads a symsrv-capable DbgHelp from `WPERF_DBGHELP_PATH`, the
application directory, or Windows SDK Debugging Tools before using the system
copy. Set `WPERF_DBGHELP_PATH` to a full DLL path or an installation directory
when multiple DbgHelp versions are installed. A fresh symbol server may take
time; further report runs use the local cache.

## Instruction heat and disassembly

```powershell
wperf annotate -i cpu.etl --symbol-path .\build\Release --limit 20
wperf annotate -i cpu.etl --symbol-path .\build\Release --disasm --json
```

`annotate --disasm` uses Zydis and Intel syntax when the build enables
disassembly. With `-DWPERF_ENABLE_DISASM=OFF`, the command falls back to
address heat with a warning.

`--json` includes `function_rva` and `instruction_rows`. When disassembly is
available, each instruction row carries RVA, bytes, text, samples, and
function-relative overhead.

## Diff

```powershell
wperf diff -1 before.etl -2 after.etl
wperf diff -1 before.etl -2 after.etl --calltree --json
wperf diff -1 before.etl -2 after.etl --pid 1234 --dso app.exe --csv
```

The default diff is weighted by CPU sample period. Known-period coverage and
normalized shares are retained so unknown periods are not presented as exact
execution time.

`--normalize weight|samples` controls share normalization. Use `--first-pid` and
`--second-pid` when the compared runs use different PIDs. JSON rows include
compatibility, profile-period, and normalization metadata. Unknown periods in
either selected trace cause normalization to fall back to samples.

`--match symbol` (default) compares resolved functions by logical module filename
and function name, ignoring build paths, timestamps and RVAs. `--match identity`
retains strict module identity and RVA matching. Unresolved addresses retain
strict identity keys in either mode and cannot promise cross-build alignment.
JSON/CSV add `matching` and `match_status` (`matched`, `added`, `removed`).
Unmapped addresses have trace-local keys rather than a shared `[unknown]` key.

## Reproducible archives

```powershell
wperf archive -i cpu.etl -o archive --symbol-path .\symbols
wperf report -i cpu.etl --archive archive
```

Archives contain manifests with module/PDB identity, SHA256, copy status, and
size-limit decisions. Modules are stored by SHA256 in `modules/`; matching PDBs
use a symbol-store layout. The ETL itself is not copied.

## Environment, providers, and recovery

```powershell
wperf doctor
wperf doctor --bundle .\bundle
wperf list
wperf list providers --max 20
wperf test
wperf recover --all-stale
```

- `doctor` reports elevation, profile sources, PMC capabilities, state, and
  recent runs.
- `doctor --bundle` writes a redacted diagnostic bundle; crash dumps require
  `--include-dumps`.
- `recover` stops stale wperf-owned ETW sessions and requires elevation.
- `test` runs a lightweight non-invasive self-test.

## Common exit statuses

| Code | Meaning |
|---:|---|
| `0` | Success |
| `2` | Invalid CLI arguments or incompatible options |
| `3` | Permission or elevation failure |
| `4` | Capture/start/cleanup failure |
| `5` | Requested PMC/LBR capability produced no data |
| `6` | Invalid trace or failed replay quality gate |
| `7` | Target process launch/open failure |
| `8` | Target process exited non-zero; original status is reported |
| `9` | Internal, IPC, filesystem, or unexpected error |

Exit codes are part of the automation contract. Treat a changed code as a
compatibility change and add a regression test.

## Copy-paste recipes

### Show hotspots above 1%

```powershell
wperf report -i app.etl --symbol-path .\symbols --percent-limit 1
```

### Profile only a process tree

```powershell
wperf report -i app.etl --pid 1234 --include-children --call-graph
```

### Compare before and after

```powershell
wperf diff -1 before.etl -2 after.etl --calltree --json
```

### Export machine-readable samples

```powershell
wperf script -i app.etl --csv -F timestamp,cpu,pid,tid,ip,comm,dso,symbol,period
```

### Verify symbols and archive identity

```powershell
wperf buildid-list -i app.etl
wperf archive -i app.etl -o archive --symbol-path .\symbols
```

### Check a hardware capability

```powershell
wperf list
wperf doctor
```

## Troubleshooting

### Capture did not become ready

Run:

```powershell
wperf doctor
wperf list
```

Check that `--elevate` was used, no stale wperf-owned session remains, and the
requested hardware source is supported. Use `wperf recover --all-stale` from an
elevated terminal for a stale wperf session.

### Symbols are missing

- pass one or more `--symbol-path` directories;
- ensure the PDB matches the module timestamp/checksum;
- use `buildid-list` to inspect identity;
- use an archive when the original binaries are unavailable.

### PMC/LBR is unavailable

`record` returns unavailable when the requested source or trigger produces no
records. Check the CPU/Windows build, privilege, existing ETW PMC ownership, and
the source IDs reported by `wperf list`. Unsupported hardware is not a successful
capture.

### JSON contains missing or approximate stacks

That is intentional. Inspect quality metadata and use `--strict-quality`,
`--exact-stacks`, or a narrower PID/time window when exact attribution is
required.
