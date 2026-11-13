# Issue #3, #4, and #5 Acceptance Status

This document tracks issue-level acceptance. Task-level work and evidence
requirements are maintained in [TODO.md](TODO.md).

## #3: Modern stacks, inline frames, and symbol cache

| Item | Implementation / validation |
|---|---|
| StackWalkRef/Def correlation | Definition opcodes 35/36 and reference opcodes 37/38 use lifetime-aware keys; raw-record regressions cover three key epochs and native `--stack-cache` configuration |
| Timestamp/checksum identity | Captured identity, PE/PDB validation, function aggregation keys, and cache identity retain timestamp/checksum fields |
| Inline frames | DbgHelp inline APIs; optimized InlineInner/InlineOuter fixture is expanded and restored after cache reopen |
| Dedicated `.symcache` | Bounded identity-aware cache with source/inline/RVA data and PE/PDB mismatch rejection |
| PDB integration | Matching/stale identity, isolated DbgHelp sessions, inline frames, and cache reopen are tested |

**#3 is closed.** Captured modern cached-stack ETLs and the full platform matrix
remain expanded validation under VAL-01/VAL-02.

## #4: Live top/trace and scheduler/I/O correlation

| Item | Implementation / validation |
|---|---|
| Persistent real-time ETW | Continuous ProcessTrace, bounded windows, lifecycle deltas, outside-lock metadata, and loss counters |
| Live trace | Shared payload decoders and incremental correlation; CI requires decoded scheduler events |
| CSwitch/ReadyThread attribution | New/old TID payload fields are resolved against thread lifetimes without using unrelated header context |
| File/disk I/O latency | Separate init/read/write/flush/completion decoding, IRP issuer, file key/name, and pairing |
| Syscall data | Enter IP, exit status, and observed durations only when identity is known |
| Live/fault integration | Elevated CI requires CPU/scheduler samples, STOP drain, worker-kill cleanup, and owned-controller cleanup |

**#4 is closed.** Normal-privilege SKIP or access-denied results do not satisfy
the live acceptance requirement.

## #5: PMC/LBR/context registers and advanced analysis

| Item | Status |
|---|---|
| LBR payload/configuration | Accepted on Intel Core i5-14600K / Windows 11 26300: timer capture produced 510 LBR events (8,194 branches) and PMC capture 29,067 events (424,434 branches), with no loss |
| Context-register CLI | Real AMD64 opcode `0x7a` payloads captured and decoded through `wperf trace -e context`; field order validated against the WMI `ContextRegistersAMD64` class and public RMP layout |
| PMC period and semantics | Accepted for ETW semantics on Intel: native source IDs/periods, per-CPU deltas, source switch/restore, and CSwitch snapshots marked exact:false |
| Zydis annotation | Implemented and tested for known function ranges |
| Weighted/call-tree diff | Implemented with period, identity, percentages, coverage, and sampling bounds |
| Archive/PDB metadata | Validated PDB GUID/age, identity/hash layout, portable mapping, and optional ETL |
| Context/LBR combined capture | Combined PMC + LBR + context capture passed (9,905 samples, 6,828 LBR events, 8,558 context payloads, no loss) |

**#5 is closed.** Intel LBR, ETW PMC semantics and AMD64 context-register
capture/decoding were hardware-validated on an Intel Core i5-14600K under
Windows 11 build 26300. Evidence is recorded in
[HARDWARE_VALIDATION.md](HARDWARE_VALIDATION.md). ARM64 context-register
decoding remains outside the accepted scope.

## Additional completed follow-up work

The current tree also implements:

- low-privilege live-top symbolization through versioned structured IPC;
- controller-side DbgHelp/PDB/archive/formatting;
- asynchronous controller rendering and STOP-responsive window transmission;
- bounded live metadata/sample queues and explicit loss/truncation counters;
- scheduler event stack capture and capability status;
- offline off-CPU blocked/ready interval correlation with instance- and module-aware grouping;
- off-CPU text, JSON, CSV, and duration-weighted folded output with truncated-boundary status;
- common report quality gates, including off-CPU lost-event and missing-stack limits.

SEC-01 and ANA-01 are implemented but remain pending re-validation against
review-4392026; this document does not mark them accepted until the re-review
evidence is recorded.

Still outside the accepted scope: external-profiler global-state atomic
restoration, full OS/CPU/VBS/WOW64 matrices, long soak and perturbation curves,
ARM64 context-register payload decoding, and JIT method lifecycle support.
