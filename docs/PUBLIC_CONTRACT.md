# Public Contract

This document defines what automation may rely on. wperf is still pre-1.0, but
changes to the items below are treated as compatibility decisions rather than
incidental refactors.

## Stable surface

The following are public:

- command names, documented option names, and their accepted values;
- success/failure semantics of documented exit codes;
- required JSON fields and the meaning of their values;
- CSV headers and column meaning;
- machine-readable enums such as `exact`, `stack_quality`, and boundary state;
- quality counters that distinguish exact, estimated, missing, unsupported, and
  incomplete data;
- the privilege model and whether a command requires elevation;
- the capability catalog emitted by `capabilities --json`;
- the structured failure object emitted by `--json-errors`;
- the success object emitted by `record --json`.

These are **not** stable:

- human-readable table layout and column widths;
- log, warning, and error message wording;
- `detail` strings inside JSON trace events;
- progress/status text on stderr;
- ordering of equal-valued rows unless explicitly documented;
- files under `%LOCALAPPDATA%\wperf`, except the documented recovery command;
- native file format internals of the symbol cache.

## Versioning policy

wperf uses four numeric components:

```text
MAJOR.MINOR.PATCH.BUILD
```

- `MAJOR`: breaking command/contract changes;
- `MINOR`: new features or additive schema fields;
- `PATCH`: compatible fixes;
- `BUILD`: release workflow run or explicit release build number.

Before 1.0, a minor release may contain a breaking change when necessary, but it
must be called out in `CHANGELOG.md`. Patch releases should remain compatible.
Additive JSON fields and new optional CLI flags do not require a major version.

## Exit codes

| Code | Contract |
|---:|---|
| `0` | The requested operation completed successfully |
| `2` | Invalid arguments or incompatible options |
| `3` | Administrator rights are required, or elevation was cancelled |
| `4` | Capture, worker, session, guard, or cleanup failure |
| `5` | Requested capability/source produced no usable data |
| `6` | Trace replay failed or a requested quality gate was not met |
| `7` | The requested target process could not be launched or opened |
| `8` | The target process exited with a non-zero status; the original status is reported separately |
| `9` | Internal error, IPC failure, filesystem failure, or an unexpected exception |

For commands that launch a target process, wperf reports a target launch failure as `7` and a non-zero target status as `8`; the original child status is emitted on stderr and must not be confused with the wperf exit code.

A caller should not infer success from report content alone: check the exit code
and, for JSON, the `quality.incomplete` field.

## Agent machine surface

`wperf capabilities --json` emits a schema-versioned catalog generated from the
same command definitions used by help and runtime validation. Command entries
identify aliases, options, elevation requirements, JSON support, global-state
side effects, examples, notes, and Agent hints.

The global `--json-errors` option replaces human error text with one JSON object
on stderr when a command fails:

```json
{
  "ok": false,
  "schema_version": 1,
  "tool_version": "0.12.0.42",
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

`error.code`, `error.exit_code`, `error.retryable`, and `error.action` are the
stable machine fields. `error.message` is diagnostic text and may change.
`--json-errors` is recognized before the command or before the `--` target
separator; after `--` it is forwarded to the target process.

`record --json` suppresses human progress output and emits one success object
with the trace path, session and correlation identity, requested capture
settings, target status, output size, and an explicit
`quality.evaluated:false`. Capture does not replay the ETL just to populate
quality metadata; consumers should use a report with `--json --metadata` and
check `quality.incomplete`.

The recommended bounded-output and artifact-first workflow is documented in
[AGENT.md](AGENT.md).

## JSON schemas

Versioned schemas live in [`schemas/`](schemas/):

- `capabilities.schema.json` — `capabilities --json`;
- `agent-error.schema.json` — `--json-errors`;
- `record.schema.json` — `record --json`;
- `report-cpu.schema.json` — `report --json --metadata`;
- `report-off-cpu.schema.json` — `report --off-cpu --json --metadata`;
- `report-io.schema.json` — `report --kind io --json --metadata`;
- `report-pmc.schema.json` — `report --kind pmc --json --metadata`;
- `diagnostic-execution.schema.json` — DPC/ISR/interrupt/driver reports;
- `diagnostic-memory.schema.json` — heap/pool reports;
- `trace.schema.json` — `trace -i FILE ... --json`;
- `trace-metadata.schema.json` — `trace -i FILE ... --json --metadata`;
- `stat.schema.json` — `stat ... --json`;
- `sched.schema.json` — `sched -i FILE --json`;
- `annotate.schema.json` — `annotate ... --json`;
- `diff.schema.json` — `diff ... --json`;
- `archive.schema.json` — `archive` manifest.json;
- `buildid-list.schema.json` — `buildid-list -i FILE --json`;
- `list.schema.json` — `list --json`;
- `doctor.schema.json` — `doctor --json`.

Schemas set `additionalProperties: true` so compatible fields can be added
without breaking existing consumers. Required fields and enum meanings are the
compatibility boundary.

JSON records may include large unsigned values (`UINT32_MAX` PIDs/TIDs,
64-bit addresses and timestamps). Consumers should use a JSON parser that
preserves 64-bit integers or parse large values as strings themselves.

## CSV contract

CSV headers are stable for documented options, including `stat`, `sched`, `buildid-list`, `annotate`, `diff`, `archive`, and `script`. New columns may be appended in a
minor release. Existing column names and meanings should not change without a
deprecation notice. Empty values mean unavailable or intentionally omitted data,
not numeric zero.

## Estimated values

Diagnostic reports use the contracts in [DIAGNOSTICS.md](DIAGNOSTICS.md).
DPC/ISR and pool are system-owned and use null PID/TID where applicable.
Execution durations are inclusive intervals, driver durations are bounded call
spans, and memory live bytes mean observed-unfreed allocations. These values
must not be silently relabeled as exclusive CPU time, minifilter/device latency,
or proven leaks. Diagnostic `--metadata` includes `quality.analysis`; unsupported
layouts and dropped analysis state affect `incomplete`. Empty selections exit 5.

- CPU sample weights are nanosecond periods. CPU/PMC sample tables, trees and
  folded output default to `--weight period`; `--weight samples` selects counts.
  Any selected unknown period causes a whole-report count fallback, identified
  by `overhead_basis` and `normalization_fallback`. Different sample domains are
  rejected unless narrowed with `--source`.
- `report --flamegraph` applies the same selection, stack, percentage and weight
  semantics as folded output, then emits one self-contained interactive HTML
  document. `--countname` changes only the displayed weight label; it does not
  rescale values. `report --flamegraph` is incompatible with `--folded`,
  `--call-graph`, `--json`, `--csv`, `--stream`, and `--metadata`.
- `wperf flamegraph -i FILE` converts standard integer-weight folded stacks to
  the same self-contained HTML format. Malformed folded data exits 6; input or
  output filesystem failures exit 9.
- `report --folded` remains a stable standard collapsed-stack output for external
  consumers such as `flamegraph.pl stacks.folded > stacks.svg`. Native HTML and
  external SVG workflows are parallel options, not replacements.
- CPU and off-CPU `report --json --metadata` include a `symbols` object with the
  effective symbol path and source, DbgHelp and symcache paths, cache-layer
  counters, unresolved counts, module totals, and interesting per-module
  diagnostics. These fields describe the local analysis host and intentionally
  retain full local paths. The DbgHelp version may be empty when unavailable.
- `--symbol-path` is repeatable and also accepts semicolon-separated values.
  Explicit paths augment `_NT_SYMBOL_PATH` instead of replacing it. The
  effective search path and both sources are visible in symbol diagnostics.
- Generated FlameGraph HTML is a local artifact. Its click, double-click, drag
  range selection, search, navigation, copy-stack, minimum-percentage, and
  SVG/PNG export interactions are presentation behavior, not JSON/CSV
  compatibility surface.
- Streaming report quality is finalized after sample consumption. With
  `stacks_loaded:false`, `stack_assessment:"not-loaded"` means no stack quality
  was evaluated, and omitted stacks are not reported as missing.
- I/O/PMC snapshot selection covers PID/TID/comm/children/CPU/time. Quality gates
  describe the full capture. Inapplicable symbol, stack, scope, weight, sort and
  percent options are errors. Empty I/O/PMC selections exit 5.
- `diff --match symbol` aligns resolved module/function names; identity mode and
  unresolved addresses retain strict build/RVA keys. Rows expose matching mode
  and added/removed/matched status.
  Unmapped addresses use trace-local keys and are not matched across traces.
- `buildid-list` PE timestamp/checksum are recorded ETL values. Current file
  metadata is in `local_identity`; status is matching/mismatched/missing.
  `--hash` requests cached SHA256 of the local file, not the captured binary.
- Image PID 0 in modern payloads denotes global ownership, including kernel
  rundown. It must not be relabeled as the collector's event-header PID.
  Delivery/decode `quality.incomplete` does not guarantee PDB symbol coverage;
  a mapped module may still be reported as module+RVA without matching symbols.
- `stat --per-thread` JSON contains observation-delta thread rows; its CSV uses
  the documented process/thread header. Observed exited threads are retained,
  discovery occurs every 50 ms, and unobserved short-lived threads may be missed.
  `threads_complete` describes access to observed counters only.
- `script -o` writes UTF-8 without BOM and LF line endings to the requested file.
- PMC interrupt periods are source-specific event counts.
- CSwitch snapshots are estimated deltas and are always `exact:false` with
  `semantics:"event-snapshot-derived"`.
- Unknown periods are not backfilled from later metadata.
- Off-CPU folded counts are confirmed wait duration in nanoseconds unless
  `--off-cpu-include-inferred` is explicitly requested.

Consumers must not reinterpret estimated or missing values as exact totals.

## Deprecation policy

When practical:

1. document the replacement and deprecation in `CHANGELOG.md` and `TODO.md`;
2. keep the old option/field for at least one subsequent release where feasible;
3. emit a warning on stderr for human-facing deprecations;
4. remove the old behavior only with an explicit compatibility decision.

Security fixes may require faster removal; the reason must be documented in the
release notes.

## Contract tests

`tests/json_contract_test.ps1` validates representative CLI output against the
schemas above. Changes to required fields, types, or enum values must update the
schemas and fixtures together. A schema change without a test change should be
treated as a review failure.
