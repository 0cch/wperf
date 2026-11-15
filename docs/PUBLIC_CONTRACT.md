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
- the privilege model and whether a command requires elevation.

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

## JSON schemas

Versioned schemas live in [`schemas/`](schemas/):

- `report-cpu.schema.json` — `report --json --metadata`;
- `report-off-cpu.schema.json` — `report --off-cpu --json --metadata`;
- `report-io.schema.json` — `report --kind io --json --metadata`;
- `report-pmc.schema.json` — `report --kind pmc --json --metadata`;
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

- CPU sample weights are nanosecond periods.
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
