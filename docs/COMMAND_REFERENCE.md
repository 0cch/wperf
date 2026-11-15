# wperf Command Reference

Generated from the same command catalog used by wperf runtime validation. Do not edit manually; run `wperf help --format markdown` instead.

## Conventions

- Commands use perf-style names and short aliases.
- Values may be separated (`-F 99`) or attached (`-F99`); long values may use `--name=value`.
- `--` ends wperf option parsing and forwards the remaining arguments to a launched command.
- `wperf help <command>` and `wperf <command> --help` render the same text help.
- System capture commands require an elevated hidden worker; offline analysis does not.

## Command index

| Command | Aliases | Purpose |
|---|---|---|
| `record` | `rec` | Capture ETW profile samples or trace events. |
| `report` | `rep` | Produce a perf-style CPU, I/O, PMC, or off-CPU report from an ETL trace. |
| `script` | `scr` | Export one line per sample. |
| `trace` | - | Print scheduler, syscall, page-fault, I/O, LBR, or context-register events. |
| `sched` | - | Summarize scheduler activity and wakeup latency. |
| `stat` | - | Collect process time, cycles and I/O counters. |
| `top` | - | Print CPU hot functions in fixed intervals. |
| `annotate` | `ann` | Show sampled instruction heat with source lines and optional disassembly. |
| `diff` | `dif` | Compare two CPU profiles; positive values indicate growth in the second trace. |
| `archive` | - | Copy referenced modules and write reproducible archive manifests. |
| `buildid-list` | - | List module identity and symbolization metadata from an ETL. |
| `list` | - | List profile sources and active sessions, or private providers. |
| `doctor` | - | Check elevation, ETW capabilities, state and recent runs. |
| `test` | - | Run a lightweight non-invasive self-test. |
| `recover` | - | Stop stale wperf-owned ETW sessions. |
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
| `-e, --event NAME\|ID` | Select a CPU profile, ETW source name or source ID. |
| `--profile NAME` | cpu, cpu-sched, sched, syscall, vm, io, kernel, all, or pmu. |
| `--pmc-source ID` | Select an ETW PMC profile source. |
| `--period N` | PMC events per interrupt (source-specific count). |
| `--pmc-counters IDS` | Counter snapshots on CSwitch (comma-separated IDs). |
| `--stack-cache` | Emit modern StackWalkRef/Def records when supported. |
| `-b, --lbr` | Enable LBR call-stack collection when supported. |
| `--context-registers` | Enable context-register collection when supported. |
| `--provider GUID` | Capture a private ETW provider session. |
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

## `report`

Produce a perf-style CPU, I/O, PMC, or off-CPU report from an ETL trace.

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
| `-s, --sort FIELD` | Sort by samples, comm, dso, or symbol. |
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
| `--kind KIND` | Select cpu, io, or pmc report. |
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
- Folded output is directly accepted by FlameGraph scripts. Counts are samples for CPU profiles and nanoseconds for off-CPU profiles.
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
| `--per-thread` | Print per-thread counters. |
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

- Without -p, stat launches the command after -- and observes that process tree.
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
| `--json` | Print JSON module identity rows. |
| `--csv` | Print CSV module identity rows. |
| `-h, --help` | Show this help. |

Examples:

```powershell
Output includes module base, size, process, PE/PDB identity, SHA256 when available, and image path.
```

## `list`

List profile sources and active sessions, or private providers.

```text
wperf list [providers] [options]
```

Elevation: not required

| Option | Description |
|---|---|
| `--max N` | Limit provider output. |
| `--json` | Print JSON profile sources, sessions, or providers. |
| `-h, --help` | Show this help. |

Examples:

```powershell
wperf list
wperf list providers --max 20
```

Notes:

- Without providers, list profile sources and active wperf-owned sessions.
- Provider enumeration can be bounded with --max.

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
