# Roadmap and Open Work

This is the public work list for wperf. Completed behavior belongs in
`CHANGELOG.md`; durable design decisions belong in `ARCHITECTURE.md`. An
item is complete only when its code, tests/evidence, documentation, and known
limitations are all recorded.

## Guiding constraints

- Stay an ETW consumer: no custom driver, kernel patch, or direct PMU MSR access.
- Keep the normal-integrity controller separated from the elevated capture
  worker.
- Never present estimated PMC values as exact totals.
- Never hide missing, ambiguous, lost, or unsupported data.
- Preserve automation contracts: JSON/CSV fields and exit codes.
- Hardware claims require reproducible evidence, not a skipped test.

## P0: Public repository readiness (completed)

### OSS-01: Project license

- [x] Selected the MIT License.
- [x] Added root `LICENSE`.
- [x] Updated README, release packaging, and third-party notices.
- [x] Confirmed the chosen license is compatible with the optional MIT Zydis/Zycore dependencies.
- [x] Documented that contributions use the MIT License and no separate CLA is required.

### OSS-02: Public contract

- [x] Documented CLI flags, exit codes, JSON/CSV, and pre-1.0 compatibility in
  [PUBLIC_CONTRACT.md](PUBLIC_CONTRACT.md).
- [x] Added versioned JSON schemas under [schemas/](schemas/).
- [x] Added `tests/json_contract_test.ps1` to CTest.
- [x] Audited documented success, unavailable, invalid-argument, capture, and
  replay-quality exit behavior.
- [x] Added deprecation guidance for renamed or removed options and fields.

### OSS-03: Contribution infrastructure

- [x] Added bug, feature, and hardware-validation issue templates.
- [x] Added `.github/pull_request_template.md`.
- [x] Added a good-first-contribution list to [CONTRIBUTING.md](../CONTRIBUTING.md).
- [x] Documented the issue, private-advisory, CI, and maintainer-review workflow.

### OSS-04: CLI command catalog and reference

- [x] Added a CLI11-backed command catalog for aliases, option validation, help,
  and Markdown generation.
- [x] Added [COMMAND_REFERENCE.md](COMMAND_REFERENCE.md) and the
  `generate-command-reference` CMake target.
- [x] Added exhaustive parser tests for every public option and alias.
- [x] Added process-level conformance tests for help parity, generated-doc
  freshness, compact values, and `--` forwarding.

## P1: Validation and robustness

### VAL-01: Platform matrix

- [ ] Windows 10 and Windows 11.
- [ ] Intel and AMD CPU families.
- [ ] VBS/HVCI on and off.
- [ ] WOW64 and multi-processor-group devices.
- [ ] Record PASS, FAIL, UNSUPPORTED, and NOT-RUN separately.

### VAL-02: Cached-stack and scheduler evidence

- [ ] Capture native StackWalkDef/Ref traffic from multiple Windows builds.
- [ ] Verify definition epochs, key reuse, unload/reload, and timestamps.
- [ ] Capture real scheduler-stack ETLs for CSwitch and ReadyThread.
- [ ] Retain expected counts, tool version, PDB identity, and quality counters.

### PMC-01: Unsupported PMC-source inventory

- [ ] Record a distinct unsupported-source case per Windows/CPU family.
- [ ] Separate unsupported hardware from failed configuration and timeout.
- [ ] Validate source ownership/conflict behavior with another ETW profiler.

### RUN-01: Soak and fault injection

- [ ] Run long captures with repeated start/stop and target churn.
- [ ] Measure controller/worker memory, handles, CPU, sessions, and cleanup.
- [ ] Inject worker loss, controller loss, STOP timeout, and session conflict.
- [ ] Keep `scripts/soak_top.ps1` usable as a starting harness.

### RUN-02: Perturbation and capacity curves

- [ ] Measure frequency/period versus core count and stack depth.
- [ ] Record sample loss, ring drops, CPU overhead, and target slowdown.
- [ ] Document where result quality degrades by workload and hardware.

### CO-01: External-profiler coexistence

- [ ] Test pre-existing PMC ownership and global interval changes.
- [ ] Never terminate unrelated sessions or overwrite unknown global state.
- [ ] Document recoverable, unknowable, and racy configurations separately.

## P2: Feature expansion

### ANA-02: JIT method lifecycle mapping

- [ ] Select the first runtime/version range.
- [ ] Capture method load/unload/rundown events.
- [ ] Resolve samples against the method lifetime active at sample time.
- [ ] Preserve unknown addresses when metadata is absent.
- [ ] Support archive/replay reproduction.

### Live context-register tracing

- [ ] Define bounded live context-register records and IPC fields.
- [ ] Add `trace --live -e context`.
- [ ] Preserve controller-side decoding and drop counters.

### ARM64 context registers

- [ ] Capture the ARM64 event-123 payload on hardware.
- [ ] Validate the field layout against Windows WMI/provider metadata.
- [ ] Add decoder tests and `trace -e context` output.
- [ ] Keep ARM64 explicitly unsupported until this is complete.

### Output and analysis improvements

- [x] Native self-contained HTML FlameGraph output and a supported external
  folded-stack workflow.
- [ ] Richer call-tree aggregation for multi-process traces.
- [x] Symbol cache diagnostics for memory, persistent `.symcache`, DbgHelp,
  module rejection, PDB state, and unresolved addresses.
- [ ] Better symbol-server proxy configuration.
- [ ] Partial-download recovery for symbol-server failures.
- [ ] Additional provider-schema rendering beyond raw ETW capture.

## Contribution entry points

Good initial contributions:

- improve documentation or examples;
- add a small sanitized ETL fixture;
- add regression coverage for a decoder boundary;
- test a supported/unsupported hardware combination;
- improve error messages without changing exit-code semantics;
- report a reproducible compatibility issue with a redacted command and quality
  counters.

Before starting a large item, open an issue describing the workflow, expected
data, and evidence plan. See [CONTRIBUTING.md](../CONTRIBUTING.md) and
[DEVELOPMENT.md](DEVELOPMENT.md).

## Implemented today

The current tree includes CPU sampling and symbols, scheduler/syscall/I/O and
off-CPU analysis, live top/trace, PMC sampling and estimated snapshots, Intel
LBR, AMD64 context-register decoding, Zydis annotation, weighted diff, verified
archives, low-privilege live symbolization, native HTML FlameGraphs, and layered
symbol-cache diagnostics. See `CHANGELOG.md` for details.
