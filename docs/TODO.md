# wperf Follow-up Work and Acceptance Plan

Updated: 2026-11-13.

This document is the central list of work that is not yet fully accepted. It
covers hardware validation, compatibility, long-running behavior, sampling
perturbation, JIT symbol support, and external-profiler coexistence.

## 1. Status and usage

The current tree includes CPU/scheduler live integration, cleanup and fault
handling, ETL replay, inline/PDB/symbol cache support, verified archives, CLI
contracts, and analysis regressions. See [ISSUE_STATUS.md](ISSUE_STATUS.md) for
issue-level status and [MAIN_FOLLOWUPS.md](MAIN_FOLLOWUPS.md) for implemented
interfaces and known limitations.

Task states are: planned, implementation complete, hardware validation
required, in progress, accepted, or platform-limited. A task is accepted only
when implementation, scoped evidence, and known limitations are all recorded.

| ID | Work | State | Priority | Main dependency |
|---|---|---|---|---|
| HW-01 | Real Intel LBR capture | Hardware validation required | P0 | Supported Intel CPU and elevated capture |
| HW-02 | PMC sampling and snapshot calibration | Hardware validation required | P0 | Exposed PMC source and calibration workload |
| HW-03 | Context-register schema and combined capture | Planned / hardware validation required | P0 | Real payload schema and supported hardware |
| VAL-01 | OS/CPU/VBS/HVCI/WOW64/group matrix | Validation expansion | P1 | Multiple systems and devices |
| VAL-02 | Real modern cached-stack ETL | Validation expansion | P1 | ETW stack-cache capable capture environment |
| RUN-01 | Long soak and fault stress | Script exists; manual run required | P1 | Elevated runner and sustained workloads |
| RUN-02 | Sampling perturbation and capacity curves | Planned / manual run required | P1 | Repeatable workload on fixed hardware |
| SEC-01 | Low-privilege live symbolization | Implemented; review re-validation pending | P1 | Structured IPC and controller-side symbols |
| ANA-01 | Off-CPU blocked/ready analysis | Implemented; review re-validation pending | P2 | Scheduler stacks and interval correlation |
| ANA-02 | JIT method lifecycle mapping | Planned | P2 | Runtime-specific ETW events |
| CO-01 | External profiler conflicts and recovery | Partial / manual validation required | P1 | Controlled competing capture tool |
| REL-01 | Continuous validation and release evidence | Partial | P1 | Specialized runners and evidence retention |

## 2. Hardware validation

### HW-01: Real Intel LBR capture

Current base: LBR configuration, payload decoding, CPU/PMC triggers, and
zero-event failure handling are implemented. Actual capture evidence from a
supported Intel platform is still required.

- [ ] Record OS build, CPU model, firmware, VBS/HVCI, logical-core layout, and privilege state.
- [ ] Use a deterministic branch workload with matching binary and PDB.
- [ ] Validate timer-triggered and PMC-triggered LBR captures independently.
- [ ] Verify from/to addresses, ordering, depth limits, PID/TID, CPU, timestamp, and module attribution.
- [ ] Keep raw evidence using the template in section 9.

Completion requires non-empty LBR data and field-level checks on a supported
target platform.

### HW-02: PMC sampling and CSwitch snapshot calibration

- [ ] Use a calibrated workload with known counter behavior.
- [ ] Validate source IDs, periods, per-CPU deltas, resets, and snapshot semantics.
- [ ] Confirm that estimated values are never presented as exact totals.
- [ ] Record unsupported sources separately from failed sources.

### HW-03: Context-register schema and combined capture

- [ ] Capture real context-register payloads on supported hardware.
- [ ] Document the ETW schema and field semantics.
- [ ] Decode only validated fields.
- [ ] Validate context registers combined with CPU/PMC sampling and LBR.

## 3. Compatibility and real stack validation

### VAL-01: Platform matrix

- [ ] Windows 10 and Windows 11.
- [ ] Intel and AMD.
- [ ] VBS/HVCI on and off.
- [ ] WOW64 and multi-processor-group systems.
- [ ] Record PASS, FAIL, UNSUPPORTED, and NOT-RUN separately.

### VAL-02: Real cached-stack ETL

- [ ] Capture StackWalkDef/Ref traffic from a native stack-cache session.
- [ ] Verify definition epochs, key reuse, module unload/reload, and timestamp matching.
- [ ] Retain the ETL, expected counts, tool version, and PDB identities.

## 4. Continuous operation and sampling overhead

### RUN-01: Soak and fault stress

- [ ] Run long captures with repeated start/stop and target churn.
- [ ] Record controller and worker memory, handles, CPU, session count, and cleanup time.
- [ ] Inject controller loss, worker termination, STOP timeout, and session conflict.

### RUN-02: Perturbation and capacity curves

- [ ] Measure frequency/period versus core count and stack depth.
- [ ] Record sample loss, ring drops, CPU overhead, and target slowdown.
- [ ] Document when result quality starts to degrade.

## 5. Low-privilege live symbolization

### SEC-01: Move DbgHelp, PDB handling, and formatting out of the elevated worker

**Status: implemented; acceptance remains pending after review-4392026 re-validation.**

Implemented:

- [x] The elevated worker owns ETW configuration, raw trace windows, metadata, quality counters, and recovery only.
- [x] Versioned structured IPC (`TOP2`, protocol version 2) transports interval, quality counters, images, processes, threads, samples, stacks, periods, sources, and PMC payloads.
- [x] Every encoded metadata and U64 payload is bounded below the pipe receive limit; the total window has an 8 MiB encoded-character budget. Field truncation increments `metadata_dropped`, and dropped samples/records are counted separately.
- [x] The normal-integrity controller owns DbgHelp, PDB lookup, symbol caches, archive loading, aggregation, formatting, and output.
- [x] `top --symbol-path` remains a controller-side option and `top --archive` supports user-provided verified modules.
- [x] The worker does not accept arbitrary file/download requests and keeps nonce, ACL, controller-PID, and process-handle validation.
- [x] Module and process metadata are preserved even when the controller cannot read a file; unsymbolized results remain explicit.
- [x] Structured live-window round-trip tests cover long Unicode metadata, oversized stack truncation, PMC payloads, and control messages.
- [x] Controller receive/control is separated from symbolization and rendering; stale render windows are bounded and reported.
- [x] Worker pipe handles are overlapped, window transmission checks the control pipe between messages, and each write has a bounded deadline.

Remaining manual hardening: slow symbol-server behavior, controller symbolizer
crash injection, and real multi-token UAC boundaries still require a
hardware/fault-injection acceptance run. Large metadata windows no longer
overflow the pipe because payloads are truncated with explicit quality
counters.

## 6. Off-CPU analysis

### ANA-01: Correlate wait intervals with stacks

**Status: implemented; acceptance remains pending after review-4392026 re-validation.**

Implemented:

- [x] Blocked means a context switch from `Wait`; ready means runnable but not running. Preemption and other switch reasons are not treated as blocking.
- [x] `sched`, `cpu-sched`, and `all` profiles request CSwitch/ReadyThread stack tracing and report scheduler-stack capability status.
- [x] Replay attaches scheduler-event stacks from exact StackWalk keys and records exact/missing/ambiguous quality.
- [x] A cross-CPU blocked/ready state machine uses CSwitch contract `tid=incoming` and `related_tid=outgoing`, and handles wakeups, duplicate ready events, TID lifetimes, migration, eviction, and truncated boundaries.
- [x] Wakeup stacks are retained separately and are never assigned as the blocked thread's stack.
- [x] `report --off-cpu` supports text, JSON, CSV, and duration-weighted folded output; folded counts are nanoseconds and inferred boundaries are excluded by default.
- [x] Aggregation keys include process instance, thread instance, module identity, and module-relative address; symbols resolve at the interval/wakeup timestamp.
- [x] Relative `--time-start`/`--time-end` crop crossing intervals; `--include-children` and `--scope user|kernel` are honored.
- [x] Off-CPU JSON/CSV expose confirmed versus truncated duration and boundary status; `--off-cpu-include-inferred` is opt-in.
- [x] Off-CPU reports run the same lost-event and missing-stack quality gates as CPU reports; strict quality also rejects truncated or evicted intervals.
- [x] Synthetic regression coverage verifies blocked/ready separation, wakeup identity, interval weights, stack retention, instance/module-aware aggregation, boundary status, relative-time cropping, and selectors.
- [x] `report --stream` remains explicitly unsupported for off-CPU because streaming does not retain stacks.

Remaining manual validation: real scheduler-stack ETLs from supported Windows
builds, controlled Sleep/lock/event/I/O workloads, interrupted captures, and
hardware acceptance evidence.

## 7. JIT symbol support

### ANA-02: Runtime-specific dynamic code lifecycle

- [ ] Select the first supported runtime and version range.
- [ ] Capture method load/unload/rundown events.
- [ ] Resolve samples against the method lifetime active at sample time.
- [ ] Preserve address/unknown rather than guessing when metadata is absent.
- [ ] Support offline archive reproduction.

## 8. External profiler coexistence and release gates

### CO-01: Conflict detection and recovery

- [ ] Test pre-existing ownership, configuration races, interval/source changes, and concurrent STOP.
- [ ] Inject configuration failures and worker/controller exits.
- [ ] Never terminate unrelated sessions or overwrite unknown global state.
- [ ] Document recoverable, unknowable, and racy state separately.

### REL-01: Continuous evidence and releases

- [ ] Keep normal and no-disassembly CI plus mandatory elevated live integration.
- [ ] Add capability-labelled hardware runners or reproducible manual evidence bundles.
- [ ] Do not count SKIP/UNSUPPORTED as PASS.
- [ ] Run long soak before releases and short fault stress in normal CI.
- [ ] Include commit, matrix, unverified ranges, performance curves, and limitations in each release.

## 9. Recommended order and evidence template

1. Complete HW-01/HW-02/HW-03 evidence and target-hardware calibration.
2. Expand VAL-01/VAL-02 and CO-01 on controlled systems.
3. Run RUN-01/RUN-02 with the new IPC and off-CPU paths.
4. Select the first ANA-02 runtime.
5. Keep REL-01 evidence attached to every supported release claim.

Each task should record:

| Field | Requirement |
|---|---|
| Task/state | ID, completed subitems, remaining subitems |
| Code | Commit, build configuration, compiler/runtime versions |
| Environment | OS build, CPU, cores/groups, VBS/HVCI, privilege, virtualization |
| Input | Workload, binary/PDB identity, reference tools |
| Operation | Reproducible commands, duration, frequency/period, filters |
| Threshold | Expected value, tolerance, scope, failure rule |
| Raw result | ETL, stdout/stderr, metrics, hash, retention location |
| Conclusion | PASS/FAIL/UNSUPPORTED/NOT-RUN and limitations |
| Sync | TODO, issue status, compatibility, architecture, and usage updates |

Raw traces may contain paths, command lines, and user information. Follow
[SECURITY.md](SECURITY.md) before sharing evidence.