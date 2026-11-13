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
| LBR payload/configuration | Decoder and CPU/PMC triggers implemented; supported Intel capture evidence is still required |
| Context-register CLI | Configured; real payload schema and decoding remain unverified |
| PMC period and semantics | Interrupt sampling and CSwitch snapshots are separate; estimates are not reported as exact totals |
| Zydis annotation | Implemented and tested for known function ranges |
| Weighted/call-tree diff | Implemented with period, identity, percentages, coverage, and sampling bounds |
| Archive/PDB metadata | Validated PDB GUID/age, identity/hash layout, portable mapping, and optional ETL |
| Context/LBR combined capture | Still requires supported Intel hardware and a verified context schema |

**#5 remains open.** The local AMD Ryzen 7 8700G environment and ordinary
privilege are not sufficient to accept Intel LBR or full context-register
support.

## Additional completed follow-up work

The current tree also implements:

- low-privilege live-top symbolization through versioned structured IPC;
- controller-side DbgHelp/PDB/archive/formatting;
- scheduler event stack capture and capability status;
- offline off-CPU blocked/ready interval correlation;
- off-CPU text, JSON, CSV, and duration-weighted folded output;
- bounded live metadata/sample queues and explicit loss counters.

Still outside the accepted scope: external-profiler global-state atomic
restoration, full OS/CPU/VBS/WOW64 matrices, long soak and perturbation curves,
real context-register payload decoding, and JIT method lifecycle support.