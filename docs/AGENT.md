# Agent Usage

wperf exposes a small machine-friendly surface for AI agents and automation.
This is intentionally an opt-in layer over the existing CLI; it does not change
the default human or Linux perf-style behavior.

## Recommended workflow

Use bounded captures and write large results to files:

```powershell
$run = "runs\job-001"
New-Item -ItemType Directory -Force $run | Out-Null

wperf capabilities --json
wperf doctor --json

wperf --json-errors record --elevate --duration 5000 `
  -o "$run\cpu.etl" `
  -- .\app.exe

wperf --json-errors report -i "$run\cpu.etl" `
  --json --metadata --strict-quality `
  -o "$run\report.json"
```

An agent should check the process exit code before consuming output, then check
`quality.incomplete` in report JSON before drawing conclusions.

## Capability discovery

```powershell
wperf capabilities --json
```

The catalog is generated from the same command definitions as help and runtime
validation. It includes:

- command names, aliases, descriptions, and usage;
- options, examples, and notes;
- whether a command requires elevation;
- whether it supports JSON;
- whether it may mutate global ETW state;
- Agent hints such as bounded-output options.

The catalog follows
[`schemas/capabilities.schema.json`](schemas/capabilities.schema.json).

## Structured errors

Add the global `--json-errors` option before the command, or before `--`:

```powershell
wperf --json-errors report
wperf report --json-errors
```

On failure, stderr contains one compact JSON line:

```json
{
  "ok": false,
  "schema_version": 1,
  "tool_version": "0.11.0.42",
  "command": "report",
  "error": {
    "code": "invalid_arguments",
    "exit_code": 2,
    "retryable": false,
    "action": "fix_arguments",
    "message": "report requires -i FILE"
  }
}
```

The `message` is diagnostic text and is not a stable API. Agents should branch
on `error.code`, `error.retryable`, and `error.action`. The output follows
[`schemas/agent-error.schema.json`](schemas/agent-error.schema.json).

`--json-errors` after `--` is forwarded to the launched target rather than
interpreted by wperf.

## Capture result

`record --json` suppresses progress text and emits one success object:

```json
{
  "ok": true,
  "schema_version": 1,
  "tool_version": "0.11.0.42",
  "command": "record",
  "status": "complete",
  "session": "wperf-...",
  "correlation_id": "...",
  "trace": "F:\\runs\\job-001\\cpu.etl",
  "profile": "cpu",
  "sample_frequency_hz": 999,
  "duration_ms": 5000,
  "target_pid": 1234,
  "target_exit_code": 0,
  "trace_size_bytes": 12345678,
  "quality": {
    "evaluated": false,
    "note": "run report with --json --metadata to evaluate trace quality"
  }
}
```

`record` does not replay the trace just to populate quality metadata. Run a
report with `--json --metadata` and inspect `quality` before relying on the
capture. The object follows
[`schemas/record.schema.json`](schemas/record.schema.json).

## Exit-code guidance

| Exit | Error code | Suggested action |
|---:|---|---|
| 2 | `invalid_arguments` | Fix arguments or incompatible options |
| 3 | `permission_required` | Request elevation or use an authorized runner |
| 4 | `capture_failed` | Run `doctor` and retry if appropriate |
| 5 | `capability_unavailable` | Run `capabilities` and check hardware/source support |
| 6 | `replay_or_quality_failed` | Inspect quality and replay diagnostics |
| 7 | `target_launch_failed` | Check the target path and process access |
| 8 | `target_failed` | Inspect the target exit status |
| 9 | `internal_error` | Collect `doctor --bundle` output before retrying |

## Output rules for agents

1. Always bound interactive or streaming commands:
   - `record`: set `--duration` unless using a terminating target command.
   - `top`: set `--iterations`.
   - `trace`: set `--max`.
   - `script`: set `--limit`.
2. Use a unique `-o` path. wperf refuses to overwrite an existing trace.
3. Write large reports to files and consume the artifact separately.
4. Use `report --json --metadata --strict-quality` for machine decisions.
5. Treat equal-valued row ordering as unspecified.
6. Do not parse human-readable tables when a JSON schema exists.
7. Use `recover --all-stale` only for stale sessions owned by wperf.

## Elevation

System capture requires an authorized elevated context. `--elevate` starts the
hidden worker but may still require a UAC interaction on an interactive machine.
For CI, use an already-authorized runner; do not expose a generic administrator
shell to an agent.

## Scope

This release adds capability discovery, structured errors, and structured
capture results. It does not yet add a long-running JSON-RPC server, MCP server,
or privileged broker. Those can be layered over the same command model when
there is a concrete need for job control or multi-client coordination.