# wperf Command Reference

Generated from the same command catalog used by wperf runtime validation. Do not edit manually; run `wperf help --format markdown` instead.

## Conventions

- Commands use perf-style names and short aliases.
- Values may be separated (`-F 99`) or attached (`-F99`); long values may use `--name=value`.
- `--` ends wperf option parsing and forwards the remaining arguments to a launched command.
- `wperf help <command>` and `wperf <command> --help` render the same text help.
- System capture commands require an elevated hidden worker; offline analysis does not.
- `--json-errors` is a global option before the command or before `--` and emits one structured error object on failure.

## Command index

| Command | Aliases | Purpose |
|---|---|---|
| `record` | `rec` | Capture ETW profile samples or trace events. |
| `report` | `rep` | Produce CPU, I/O, PMC, off-CPU, interrupt, memory, or driver reports from an ETL trace. |
| `script` | `scr` | Export one line per sample. |
| `trace` | - | Print scheduler, syscall, page-fault, I/O, LBR, or context-register events. |
| `sched` | - | Summarize scheduler activity and wakeup latency. |
| `stat` | - | Collect process time, cycles and I/O counters. |
| `top` | - | Print CPU hot functions in fixed intervals. |
| `annotate` | `ann` | Show sampled instruction heat with source lines and optional disassembly. |
| `diff` | `dif` | Compare two CPU profiles; positive values indicate growth in the second trace. |
| `archive` | - | Copy referenced modules and write reproducible archive manifests. |
| `buildid-list` | - | List module identity and symbolization metadata from an ETL. |
| `list` | - | List profile sources, sessions, private providers, or pool tag snapshots. |
| `doctor` | - | Check elevation, ETW capabilities, state and recent runs. |
| `test` | - | Run a lightweight non-invasive self-test. |
| `recover` | - | Stop stale wperf-owned ETW sessions. |
| `capabilities` | - | Print the machine-readable command and Agent capability catalog. |
| `help` | - | Show general help or help for one command. |
| `version` | - | Print the wperf version. |

## Commands

## `record`

Capture ETW profile samples or trace events.

```text
wperf record [options] [--] [command [arguments...]]
```

Elevation: required

Aliases: `rec`

| Option | Description |
|---|---|
| `--elevate` | Start the hidden elevated capture worker. |
| `-F, --freq HZ` | Sampling frequency, 1..100000 (default: 1000). |
| `-g, --call-graph` | Capture call stacks (default for system ETW). |
| `--no-call-graph` | Disable system ETW stack tracing. |
| `-p, --pid PID` | Attach to an existing process. |
| `-d, --duration MS` | Capture duration in milliseconds; default is until Ctrl-C. |
| `-o, --output FILE` | Write the ETL trace to FILE. |
| `--json` | Print one success object with capture identity and artifact metadata. |
| `-e, --event NAME\|ID` | Select a CPU profile, ETW source name or source ID. |
| `--profile NAME` | cpu, cpu-sched, sched, syscall, vm, io, kernel, all, pmu, dpc, isr, heap, pool, or driver. |
| `--pmc-source ID` | Select an ETW PMC profile source. |
| `--period N` | PMC events per interrupt (source-specific count). |
| `--pmc-counters IDS` | Counter snapshots on CSwitch (comma-separated IDs). |
| `--stack-cache` | Emit modern StackWalkRef/Def records when supported. |
| `-b, --lbr` | Enable LBR call-stack collection when supported. |
| `--context-registers` | Enable context-register collection when supported. |
| `--provider GUID` | Capture a private ETW provider session. |
| `--pool-tag TAG` | Required with --profile pool: four printable ASCII characters, including spaces. |
| `--session NAME` | Use an explicit ETW session name. |
| `-h, --help` | Show this help. |

Examples:

```powershell
wperf record --elevate -F 999 -g --duration 5000 -o cpu.etl
wperf record --elevate -g -o child.etl -- .\app.exe --input data.txt
```

Notes:

- System capture requires administrator rights; --elevate starts one hidden elevated worker.
- System ETW call stacks are enabled by default. Use --no-call-graph to disable them.
- cpu-sched, sched, and all also request CSwitch/ReadyThread stack tracing.
- PMC interrupt sampling and --pmc-counters CSwitch snapshots require separate captures.
- heap requires --pid of an existing process; only allocations after attachment are observed.
- pool requires --pool-tag to bound high-rate capture; list --pool-tags shows current tag usage.
- dpc/isr/heap/pool/driver are opt-in event captures; -F, PMC, LBR and private providers cannot be combined with them.

## `report`

Produce CPU, I/O, PMC, off-CPU, interrupt, memory, or driver reports from an ETL trace.

```text
wperf report -i FILE [options]
```

Elevation: not required

Aliases: `rep`

| Option | Description |
|---|---|
| `-i, --input FILE` | Read FILE. |
| `-o, --output FILE` | Write the selected report format to FILE. |
| `-p, --pid PID` | Include only PID. |
| `-t, --tid TID` | Include only TID. |
| `--comm TEXT` | Filter by process name substring. |
| `--dso TEXT` | Filter by module/shared-object substring. |
| `--symbol TEXT` | Filter by symbol substring. |
| `--symbol-path DIR` | Add a local PDB search directory. |
| `--archive DIR` | Resolve unavailable paths through verified modules.tsv. |
| `--percent-limit N` | Hide rows below N percent. |
| `-s, --sort FIELD` | Sort by weight, samples, comm, dso, or symbol. |
| `--weight MODE` | CPU/PMC sample weights: period (default) or samples. |
| `--source ID` | Select one CPU/PMC sample source or PMC snapshot counter. |
| `-g, --call-graph MODE` | Text call tree; MODE is callee or caller. |
| `--metadata` | With --json, include quality metadata and rows. |
| `--strict-quality` | Fail on lost, dropped, or malformed data. |
| `--max-lost-events N` | Maximum acceptable ETW lost-event count. |
| `--max-missing-stacks N` | Maximum acceptable missing-stack count. |
| `--exact-stacks` | Include only exact stack correlations. |
| `--stream` | Two-pass CPU table; omits stacks and retains no samples. |
| `--include-children` | Include descendants of --pid. |
| `--cpu N` | Select processor index. |
| `--time-start MS` | Select data after capture-relative time. |
| `--time-end MS` | Select data before capture-relative time. |
| `--scope SCOPE` | Select all, user, or kernel address space. |
| `--folded, --collapsed` | Print collapsed stacks with counts. |
| `--off-cpu` | Report blocked/ready intervals weighted by wait time. |
| `--off-cpu-include-inferred` | Include end-of-capture truncated intervals. |
| `--stdio` | Text output (default). |
| `--quiet` | Suppress quality diagnostics. |
| `--verbose` | Print complete quality counters to stderr. |
| `--csv` | Print CSV rows. |
| `--json` | Print JSON rows. |
| `--kind KIND` | Select cpu, io, pmc, dpc, isr, interrupt, heap, pool, or driver report. |
| `-h, --help` | Show this help. |

Examples:

```powershell
wperf report -i cpu.etl --symbol-path .\build\Release --call-graph
wperf report -i cpu.etl --json --metadata
wperf report -i sched.etl --off-cpu --folded --quiet -o offcpu.folded
```

Notes:

- Quality diagnostics go to stderr; --quiet suppresses them and --verbose prints every counter.
- --folded/--collapsed and --call-graph cannot be combined with --json or --csv.
- CPU overhead and folded output use periods (CPU ns or PMC events); --weight samples uses counts. Unknown periods fall back to counts with a warning.
- Mixed sample sources require --source ID. Off-CPU weights remain nanoseconds.
- I/O and PMC snapshots support PID/TID/comm/children/CPU/time selection and loss/strict gates; symbol, stack, scope, weight, sort and percent options are rejected.
- PMC snapshots also support --csv; I/O supports text/JSON. --max-missing-stacks requires loaded CPU/off-CPU or allocation stacks.
- Diagnostic kinds support text/JSON/CSV, symbols, archives, CPU/time selectors and quality gates; graph, stream, weight, source, scope, sort and percent options are rejected.
- DPC/ISR/interrupt/pool are system-owned and reject PID/TID/comm/children filters. Heap/driver support these selectors.
- Heap/pool time and thread/CPU filters select allocation cohorts; releases through the selected end are always consumed. Observed live bytes do not prove a leak.
- -o writes UTF-8 without BOM and LF line endings.

## `script`

Export one line per sample.

```text
wperf script -i FILE [options]
```

Elevation: not required

Aliases: `scr`

| Option | Description |
|---|---|
| `-i, --input FILE` | Read FILE. |
| `-o, --output FILE` | Write output to FILE. |
| `-p, --pid PID` | Include only PID. |
| `-F, --fields LIST` | Comma-separated fields (default: timestamp,cpu,pid,tid,ip,comm,dso,symbol). |
| `--symbol-path DIR` | Add a local PDB search directory. |
| `--archive DIR` | Resolve unavailable paths through verified modules.tsv. |
| `--limit N` | Emit at most N samples; 0 means unlimited. |
| `--csv` | Use comma-separated output and CSV quoting. |
| `-h, --help` | Show this help. |

Examples:

```powershell
wperf script -i cpu.etl --limit 20
wperf script -i cpu.etl -F timestamp,pid,comm,symbol --csv
```

Notes:

- Supported fields: timestamp, cpu, pid, tid, ip, comm, dso, symbol, stack, source, period, and stack_quality.
- Use --csv for comma-separated output; otherwise each field is separated by a space.

## `trace`

Print scheduler, syscall, page-fault, I/O, LBR, or context-register events.

```text
wperf trace -i FILE [options] | wperf trace --live [options]
```

Elevation: not required

| Option | Description |
|---|---|
| `-i, --input FILE` | Read an offline ETL trace. |
| `--live` | Capture and print events in real time. |
| `-e, --event KIND` | sched, syscall, pagefault, io, lbr, or context (offline). |
| `-p, --pid PID` | Include only PID. |
| `--max N` | Limit emitted events; 0 means unlimited (default). |
| `--duration MS` | Live capture duration in milliseconds. |
| `--json` | Print JSON events. |
| `--metadata` | Wrap JSON events with quality and truncation metadata. |
| `--elevate` | Start the hidden elevated worker for --live. |
| `-h, --help` | Show this help. |

Examples:

```powershell
wperf trace -i trace.etl -e sched --max 20
wperf trace --live --elevate -e sched --duration 5000
wperf trace -i trace.etl -e context --json
```

Notes:

- Offline replay reads -i FILE. --live captures events in real time and may require --elevate.
- Event kinds are sched, syscall, pagefault, io, lbr, and context.

## `sched`

Summarize scheduler activity and wakeup latency.

```text
wperf sched -i FILE [options]
```

Elevation: not required

| Option | Description |
|---|---|
| `-i, --input FILE` | Read FILE. |
| `--json` | Print JSON scheduler rows and quality metadata. |
| `--csv` | Print CSV scheduler rows. |
| `-h, --help` | Show this help. |

Examples:

```powershell
wperf sched -i sched.etl
```

## `stat`

Collect process time, cycles and I/O counters.

```text
wperf stat [options] [-p PID | -- command [arguments...]]
```

Elevation: not required

| Option | Description |
|---|---|
| `-p, --pid PID` | Observe an existing process. |
| `--duration MS` | Observation duration (default: 1000). |
| `--per-thread` | Include observation-delta thread counters in text, JSON, or CSV. |
| `--interval MS` | Sample interval for repeated process statistics. |
| `--pmu` | Use an ETW PMC profile source. |
| `-e, --event SOURCE` | Select a PMC source name or numeric ID. |
| `--period N` | PMC events per interrupt (default: 1000000). |
| `--json` | Print JSON process statistics. |
| `--csv` | Print CSV process statistics. |
| `-h, --help` | Show this help. |

Examples:

```powershell
wperf stat -p 1234 --duration 2000
wperf stat --pmu -e TotalCycles -- .\app.exe
```

Notes:

- Without -p, stat launches the command after -- and observes its root process.
- --per-thread discovers threads every 50 ms, retains observed exited threads, and reports deltas since observation. Short-lived unobserved threads may be missed.
- --per-thread JSON adds threads and threads_complete; CSV uses process/thread row_type records. PMC mode does not support --per-thread.
- PMC mode uses an ETW profile source; it does not access PMU MSRs directly.

## `top`

Print CPU hot functions in fixed intervals.

```text
wperf top [options]
```

Elevation: required

| Option | Description |
|---|---|
| `-p, --pid PID` | Include only PID. |
| `-e, --event SOURCE` | cpu or an ETW PMC source. |
| `--interval MS` | Refresh interval (default: 1000). |
| `--iterations N` | Number of snapshots; 0 means until Ctrl-C. |
| `--sort FIELD` | Sort by samples, comm, dso, or symbol. |
| `--symbol-path DIR` | Add a local PDB search directory. |
| `--archive DIR` | Resolve unavailable paths through verified modules.tsv. |
| `--percent-limit N` | Hide rows below N percent (default: 1). |
| `--elevate` | Start the hidden elevated worker. |
| `-h, --help` | Show this help. |

Examples:

```powershell
wperf top --elevate --interval 1000
```

Notes:

- System capture requires administrator rights. --elevate keeps the controller at normal integrity.

## `annotate`

Show sampled instruction heat with source lines and optional disassembly.

```text
wperf annotate -i FILE [options]
```

Elevation: not required

Aliases: `ann`

| Option | Description |
|---|---|
| `-i, --input FILE` | Read FILE. |
| `--symbol-path DIR` | Add a local PDB search directory. |
| `--archive DIR` | Resolve unavailable paths through verified modules.tsv. |
| `-p, --pid PID` | Include only PID. |
| `--limit N` | Limit rows (default: 50). |
| `--disasm` | Decode instruction bytes with Zydis. |
| `--stdio` | Text output (default). |
| `--json` | Print JSON rows. |
| `--csv` | Print CSV rows. |
| `-h, --help` | Show this help. |

Examples:

```powershell
wperf annotate -i cpu.etl --symbol-path .\build\Release --limit 20
wperf annotate -i cpu.etl --disasm --json
```

Notes:

- Text output is the default. --disasm adds Zydis instruction decoding when available.
- Symbols require matching PDB files or a verified archive created by wperf archive.

## `diff`

Compare two CPU profiles; positive values indicate growth in the second trace.

```text
wperf diff -1 FIRST.etl -2 SECOND.etl [options]
```

Elevation: not required

Aliases: `dif`

| Option | Description |
|---|---|
| `-1, --first FILE` | Read the baseline profile from FILE. |
| `-2, --second FILE` | Read the comparison profile from FILE. |
| `--symbol-path DIR` | Add a local PDB search directory. |
| `--archive DIR` | Resolve unavailable paths through verified modules.tsv. |
| `--pid PID` | Include only PID. |
| `--dso TEXT` | Filter by module/shared-object substring. |
| `--first-pid PID` | PID filter for the first trace. |
| `--second-pid PID` | PID filter for the second trace. |
| `--normalize MODE` | Normalize shares by weight or samples (default: weight). |
| `--match MODE` | Match by module/function symbol (default) or strict identity. |
| `--calltree, --weighted` | Compare call-tree weights. |
| `--json` | Print JSON rows. |
| `--csv` | Print CSV rows. |
| `-h, --help` | Show this help. |

Examples:

```powershell
wperf diff -1 before.etl -2 after.etl --calltree --json
```

Notes:

- Positive values indicate growth in the second profile; negative values indicate reduction.
- --calltree and --weighted are equivalent.
- Symbol matching ignores build paths, timestamps and RVAs for resolved functions. Unresolved addresses retain strict identity keys; unmapped IPs use trace-local keys. match_status identifies added/removed rows.

## `archive`

Copy referenced modules and write reproducible archive manifests.

```text
wperf archive -i FILE -o DIRECTORY [options]
```

Elevation: not required

| Option | Description |
|---|---|
| `-i, --input FILE` | Read FILE. |
| `-o, --output DIRECTORY` | Write the archive to DIRECTORY. |
| `-p, --pid PID` | Include only modules referenced by PID. |
| `--symbol-path DIR` | Add a local PDB search directory. |
| `--max-size-mb N` | Skip copied files larger than N MiB. |
| `--include-trace` | Copy the input ETL as trace.etl. |
| `-h, --help` | Show this help. |

Examples:

```powershell
The archive directory contains copied modules plus manifest.json, manifest.csv, and manifest.txt.
Use --include-trace to copy the input ETL as trace.etl for a self-contained artifact.
```

## `buildid-list`

List module identity and symbolization metadata from an ETL.

```text
wperf buildid-list -i FILE [options]
```

Elevation: not required

| Option | Description |
|---|---|
| `-i, --input FILE` | Read FILE. |
| `--hash` | Hash each distinct local module file once (SHA256; optional). |
| `--json` | Print JSON module identity rows. |
| `--csv` | Print CSV module identity rows. |
| `-h, --help` | Show this help. |

Examples:

```powershell
Output preserves ETL PE timestamp/checksum and mapping rows, and separates recorded_identity from local_identity.
Status is matching, mismatched, or missing. --hash adds local-file SHA256; without it hash_status is not-requested.
```

## `list`

List profile sources, sessions, private providers, or pool tag snapshots.

```text
wperf list [providers] [options]
```

Elevation: not required

| Option | Description |
|---|---|
| `--max N` | Limit provider or pool-tag output. |
| `--json` | Print JSON profile sources, sessions, providers, or pool tags. |
| `--pool-tags` | List current system pool tag counters; use --max to limit rows. |
| `-h, --help` | Show this help. |

Examples:

```powershell
wperf list
wperf list providers --max 20
```

Notes:

- Without providers or --pool-tags, list profile sources and active wperf-owned sessions.
- Provider and pool-tag enumeration can be bounded with --max. Pool counters are snapshots and allocation/free counters may wrap.

## `doctor`

Check elevation, ETW capabilities, state and recent runs.

```text
wperf doctor [options]
```

Elevation: not required

| Option | Description |
|---|---|
| `--bundle DIR` | Write a diagnostic bundle to DIR. |
| `--include-dumps` | Include .dmp files in the bundle. |
| `--json` | Print JSON diagnostics. |
| `-h, --help` | Show this help. |

Examples:

```powershell
--bundle writes a redacted diagnostic bundle. Crash dumps are included only with --include-dumps.
```

## `test`

Run a lightweight non-invasive self-test.

```text
wperf test
```

Elevation: not required

| Option | Description |
|---|---|
| `-h, --help` | Show this help. |

## `recover`

Stop stale wperf-owned ETW sessions.

```text
wperf recover (--session NAME | --all-stale)
```

Elevation: required

| Option | Description |
|---|---|
| `--session NAME` | Stop one explicit ETW session. |
| `--all-stale` | Stop sessions referenced by stale wperf state. |
| `-h, --help` | Show this help. |

Examples:

```powershell
Recovery requires elevation and only stops ETW sessions recorded as wperf-owned stale state.
```

## `capabilities`

Print the machine-readable command and Agent capability catalog.

```text
wperf capabilities [--json]
```

Elevation: not required

| Option | Description |
|---|---|
| `--json` | Print the capability catalog as JSON. |
| `-h, --help` | Show this help. |

Examples:

```powershell
wperf capabilities --json
```

Notes:

- The catalog is generated from the same command definitions as help and runtime validation.
- Use it to discover commands, options, aliases, elevation requirements, and Agent hints.

## `help`

Show general help or help for one command.

```text
wperf help [command] [--format text|markdown]
```

Elevation: not required

| Option | Description |
|---|---|
| `--format FORMAT` | Output format: text or markdown (default: text). |
| `-h, --help` | Show this help. |

Examples:

```powershell
wperf help
wperf help report
wperf help --format markdown
```

Notes:

- Markdown output is generated from the same command catalog used by runtime validation.
- Regenerate docs/COMMAND_REFERENCE.md with: wperf help --format markdown.

## `version`

Print the wperf version.

```text
wperf version
```

Elevation: not required

| Option | Description |
|---|---|
| `-h, --help` | Show this help. |
