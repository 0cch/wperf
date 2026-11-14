# Development Guide

This guide is for contributors working on wperf itself. User-facing command
behavior is documented in [USAGE.md](USAGE.md); system design is in
[ARCHITECTURE.md](ARCHITECTURE.md).

## Choose a contribution

| If you want to... | Start here |
|---|---|
| Fix a capture/replay bug | `src/etl_reader.cpp`, `src/etw_decode.cpp`, regression tests |
| Add a command or option | `src/commands/`, `CMakeLists.txt`, USAGE |
| Improve symbolization | `src/symbolizer.cpp`, fixture tests |
| Work on live top/trace | `src/live_top.cpp`, `src/live_data.cpp`, live IPC tests |
| Improve reports | `src/cpu_report.cpp`, `src/off_cpu_report.cpp`, `src/analysis.cpp` |
| Improve documentation | README, USAGE, DEVELOPMENT, ARCHITECTURE, TODO |
| Validate hardware | hardware issue template + `HARDWARE_VALIDATION.md` |

## First 30 minutes

1. Build and run the normal test suite.
2. Run `wperf doctor` and `wperf --version` from the build directory.
3. Read [ARCHITECTURE.md](ARCHITECTURE.md) for the controller/worker/guard model.
4. Pick a small item from [TODO.md](TODO.md) or a good-first issue.
5. Add a focused test before changing behavior.
6. Run the normal tests and, if capture changed, the elevated live tests.

## Architecture in 60 seconds

`wperf` starts an ETW session in an elevated worker, transfers bounded raw
events to a normal-integrity controller, and decodes them into one `TraceData`
model. Offline ETL replay uses the same model. Reports, scripts, annotation,
diff, and live views consume that model; symbolization and PDB handling stay
in the controller. The elevated worker never performs arbitrary file parsing.

## Prerequisites

- Windows 10 21H2+ or Windows 11, x64.
- Visual Studio 2022 with Desktop C++ and a recent Windows SDK.
- CMake 3.24+.
- PowerShell 7 for scripts and the best quoting experience.
- Optional: Windows Performance Toolkit (`xperf`, WPA) for independent ETW
  inspection.
- Optional: Debugging Tools for Windows (`cdb`) for crash investigation.

The first configure with disassembly enabled fetches pinned Zydis/Zycore
revisions from GitHub. To work fully offline, provide a pre-populated CMake
FetchContent cache or build with `-DWPERF_ENABLE_DISASM=OFF`.

## Build and test

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

No-disassembly build:

```powershell
cmake -S . -B build-off `
  -G "Visual Studio 17 2022" -A x64 `
  -DWPERF_ENABLE_DISASM=OFF
cmake --build build-off --config Release --parallel
ctest --test-dir build-off -C Release --output-on-failure
```

The normal CTest run skips the two live ETW tests when the current process is not
elevated. To require and run them:

```powershell
$env:WPERF_REQUIRE_LIVE_TESTS = "1"
ctest --test-dir build -C Release `
  -R "live_integration|live_top_controller_smoke" `
  --output-on-failure
```

Run this from an elevated PowerShell terminal. CI uses the same requirement so
an access-denied or empty capture cannot be mistaken for a pass.

## Test strategy

| Layer | Location | Purpose |
|---|---|---|
| Unit | `tests/unit_tests.cpp` | Decoders, aggregation, symbol/cache primitives |
| Regression | `tests/regression_tests.cpp` | Raw event replay, quality gates, IPC, report contracts |
| CLI | `CMakeLists.txt` `add_test` entries | Option validation and checked-in ETL fixtures |
| Live integration | `tests/live_integration_tests.cpp` | Real ETW capture, STOP, orphan cleanup |
| Live controller smoke | `tests/live_top_smoke.ps1` | Worker/controller decode and rendering |
| Replay contract | `scripts/replay_contract.ps1` | Stable report/JSON contract on fixture data |

Checked-in fixtures live in `tests/data/` with `*.expected.json` contracts.
Keep fixture bytes stable unless the change explicitly requires a new trace.
Raw traces are compressed ETLs and may contain machine/process metadata; review
redaction before adding new fixtures.

### What to test for a decoder change

- valid payload boundary and every variable-length field;
- every rejection path for truncated/oversized input;
- unknown opcode/provider behavior;
- quality counter increments;
- replay output regression using an existing fixture or a synthetic
  `EVENT_RECORD` sequence;
- JSON/CSV field compatibility where visible.

### What to test for a live change

- READY handshake and child launch ordering;
- normal STOP and bounded STOP;
- worker death, controller death, and guard cleanup;
- session ownership and restore behavior;
- ring/metadata drop counters;
- no unrelated ETW session is stopped.

## Source map

- `src/commands.cpp` and `src/commands/*` — CLI parsing/dispatch.
- `src/etw.cpp` — logger lifecycle, profile source/interval configuration,
  LBR/context/PCM setup, restore paths.
- `src/etl_reader.cpp` — ETL replay, correlation, StackWalk/cached stacks,
  off-CPU interval construction.
- `src/etw_decode.cpp` — classic/kernel/profile payload decoders.
- `src/trace_model.*`, `src/trace_events.*` — shared trace model and event
  enrichment.
- `src/analysis.cpp`, `src/cpu_report.cpp`, `src/off_cpu_report.cpp` — analysis
  and report shaping.
- `src/symbolizer.cpp` — DbgHelp sessions, PDB identity, inline frames, cache.
- `src/live_top.cpp`, `src/live_data.cpp`, `src/commands/live_worker.cpp` —
  live capture and IPC.
- `src/elevation.cpp`, `src/ipc.cpp`, `src/session_guard.cpp` — privilege and
  recovery boundary.
- `src/disassembler.cpp` — optional Zydis integration.

## Adding a command or option

1. Extend the dispatcher/parser in `src/commands.cpp` or the relevant
   `src/commands/*.cpp` file.
2. Add help text in `src/commands/help.cpp`.
3. Return a documented exit code and route errors to stderr.
4. Add CLI validation tests in `CMakeLists.txt`.
5. Add a replay/regression test if the command changes analysis output.
6. Update [USAGE.md](USAGE.md) and the compatibility notes if the behavior is
   platform-dependent.

Do not add a new parser merely to inspect an existing ETL. Reuse
`ReadEtwFile`/the shared `TraceData` model.

## Adding an ETW decoder or hardware capability

- Follow the Windows SDK structure layout exactly; do not infer pointer size
  from the host unless the event header is unavailable.
- Validate `UserDataLength` before every read.
- Preserve unknown/unavailable data explicitly.
- Keep estimated values separate from exact counters.
- For undocumented or version-sensitive payloads, document the source, hardware
  build, and field-level evidence before exposing a field.
- Add a synthetic replay test even when a real hardware test is also available.
- Capture real evidence with loss counters and retain it privately if it contains
  sensitive data.

See [HARDWARE_VALIDATION.md](HARDWARE_VALIDATION.md) for the current Intel
evidence and the expected level of detail.

## Good implementation habits

- Prefer explicit ownership and bounded containers in the elevated worker.
- Avoid unbounded work in ETW callbacks.
- Keep formatting out of decoders.
- Avoid copying large stacks/payloads unnecessarily.
- Do not silently repair missing data; count it and expose quality.
- Treat PID/TID reuse as a real case.
- Keep JSON/CSV field names and exit codes stable.
- Use `std::filesystem` and Windows wide APIs consistently.
- Build with `/W4 /permissive-`; new warnings should be fixed, not suppressed.

### Error-path checklist

- Does a failed start restore the previous sampling interval/source?
- Does a failed stop leave enough state for `recover`?
- Are temporary sessions, handles, and files released on every exit?
- Does the controller exit if its worker disappears?
- Does the worker exit if its controller disappears?
- Are partial traces marked incomplete rather than reported as clean?
## Debugging

### Local state and logs

wperf stores run state, logs, crash metadata, and symbol caches under
`%LOCALAPPDATA%\wperf` by default.

```powershell
wperf doctor
wperf doctor --bundle .\bundle
```

The diagnostic bundle excludes crash dumps unless `--include-dumps` is passed.
Review the bundle before sharing; ETLs and logs can contain sensitive process
metadata.

### Debugger workflow

Release builds include PDBs. A practical loop is:

```powershell
cdb -y .\build\Release -o `
  -c ".reload; sxe av; g; .ecxr; kb; q" `
  .\build\Release\wperf.exe report -i trace.etl --verbose
```

Use a small fixture whenever possible. If the failure only appears on a large
real trace, preserve the ETL privately and reduce it before sharing.

### ETW inspection

Use `xperf`/WPA as an independent consumer:

```powershell
xperf -i trace.etl -o trace.dump.csv -tle -tti -a dumper
xperf -i trace.etl -o stats.txt -tle -tti -a tracestats -detail storage
```

These tools are especially useful for checking provider GUID/opcode counts,
lost buffers, payload lengths, and whether a failure is in capture or replay.

### Common failure classes

- **Capture not ready**: privilege, session conflict, unsupported profile
  source, or hardware capability.
- **Empty requested result**: PMC/LBR configuration succeeded but no matching
  events were emitted.
- **Quality gate failure**: expected when comparing a complete result against a
  truncated trace; inspect the quality metadata before changing the gate.
- **Symbol mismatch**: module timestamp/checksum and PDB identity differ; use
  `buildid-list`, a matching PDB, or a verified archive.
- **Stale session**: use elevated `wperf recover --all-stale`, then check
  `doctor`.

## Release workflow

Versions are four-part numeric values: `MAJOR.MINOR.PATCH.BUILD`.
`scripts/build_release.ps1` builds and tests the binary and packages:

- `wperf.exe`
- `wperf.pdb`
- `manifest.json`
- `SHA256SUMS.txt`
- README, changelog, LICENSE, contributing/security guides, notices, and durable docs

The GitHub workflow triggers on `vMAJOR.MINOR.PATCH` tags and publishes a
release with the GitHub run number as the fourth component. A full four-part
tag is accepted as an explicit override. See [RELEASE.md](RELEASE.md).

Before tagging, run the normal CTest suite and the elevated live tests. Hardware
claims must already have evidence in [HARDWARE_VALIDATION.md](HARDWARE_VALIDATION.md).

## Documentation maintenance

Durable documentation is intentionally small:

- [README](../README.md) — project overview and first workflow;
- [USAGE.md](USAGE.md) — user-facing command behavior;
- [PUBLIC_CONTRACT.md](PUBLIC_CONTRACT.md) — CLI, exit-code, JSON/CSV and
  schema compatibility policy;
- [schemas/](schemas/) — versioned machine-readable output contracts;
- [ARCHITECTURE.md](ARCHITECTURE.md) — system design and trust boundaries;
- [COMPATIBILITY.md](COMPATIBILITY.md) — platform/semantic limits;
- [TODO.md](TODO.md) — open work and acceptance criteria;
- [HARDWARE_VALIDATION.md](HARDWARE_VALIDATION.md) — hardware evidence.

Do not add historical review transcripts, issue-by-issue acceptance diaries, or
milestone process logs. Put completed behavior in the changelog, unresolved work
in TODO, and durable technical rationale in the architecture guide.

## Where help is needed

The highest-value contribution areas are tracked in [TODO.md](TODO.md):

- platform coverage: Windows 10, AMD, VBS/HVCI, WOW64, multi-group;
- real cached-stack and scheduler-stack traces from more Windows builds;
- sanitized replay fixtures and decoder boundary tests;
- hardware validation reports for PMC/LBR/context-register behavior;
- soak, fault-injection, and sampling-overhead measurements;
- documentation examples and troubleshooting.

## Definition of done

- [ ] The behavior and its limitations are described.
- [ ] A focused test or hardware evidence exists.
- [ ] Normal CTest passes.
- [ ] Elevated live tests pass when capture behavior changed.
- [ ] JSON/CSV/exit-code compatibility is considered.
- [ ] Relevant durable docs are updated.
- [ ] No sensitive evidence or machine-specific paths are committed.

## Review standards

Reviewers should block changes that hide data loss, merge incompatible
process/thread lifetimes, bypass the controller/worker boundary, change
machine-readable semantics without documentation, or claim hardware support
without reproducible evidence.

## Contribution workflow

1. Open or reference an issue for non-trivial work.
2. Create a focused branch (`codex/...` or your own convention).
3. Make the smallest complete change.
4. Add tests and evidence.
5. Run normal and elevated tests as applicable.
6. Update the durable docs.
7. Submit a PR with the exact commands/results used.

See [CONTRIBUTING.md](../CONTRIBUTING.md) for reporting and PR checklists.
