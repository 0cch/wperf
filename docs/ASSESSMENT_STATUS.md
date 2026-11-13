# wperf Assessment Follow-up Status

This is an English summary of the historical assessment. Current task-level
status is maintained in [TODO.md](TODO.md), and issue acceptance is maintained
in [ISSUE_STATUS.md](ISSUE_STATUS.md).

## Overall result

The original assessment identified 14 concrete problem areas. The current tree
implements the directly actionable fixes in sampling periods, full module
identity, archive loading, streaming replay, stack interning, inline symbols,
symbol caching, PMC snapshots, metadata locking, managed budgets, early
recovery checkpoints, zero-event validation, controller handles, teardown
deadlines, report selectors, and quantile analysis.

The remaining work is primarily target-hardware evidence rather than missing
core plumbing.

## Resolved findings

- Sample periods are preserved explicitly and are not backfilled by later metadata.
- Module-version identity participates in aggregation keys and cache identity.
- Stack references are lifetime-aware and bounded.
- Inline frames survive cache reopen and identity validation.
- Live top symbolization runs in the ordinary-permission controller, not the elevated worker.
- Scheduler stack capture and off-CPU blocked/ready correlation are implemented.
- Repeated milestone smoke scripts were removed; focused CTest coverage is authoritative.

## Remaining validation gaps

| Area | Remaining work |
|---|---|
| Intel LBR | Capture on supported Intel hardware and verify branch fields |
| PMC calibration | Known-counter workload, source/period validation, and reset behavior |
| Context registers | Real payload schema and decoding |
| Platform matrix | OS/CPU/VBS/HVCI/WOW64/processor-group coverage |
| Real cached stacks | Native StackWalkDef/Ref ETL evidence |
| Soak and perturbation | Long runs, fault injection, overhead/loss curves |
| JIT | Runtime-specific method lifecycle mapping |
| External profilers | Controlled conflict and recovery testing |

## Current verification

- Stable CTest coverage includes CLI contracts, replay, archive, regression,
  folded output, off-CPU output, and live integration.
- CI runs both Zydis-enabled and Zydis-disabled Release builds.
- CI sets `WPERF_REQUIRE_LIVE_TESTS=1`, so an unexpected live-test skip fails.
- Hardware-dependent claims remain NOT-RUN/UNSUPPORTED until target evidence is attached.

The project does not claim exact Linux perf equivalence for event groups,
multiplex scaling, syscall argument semantics, context-register payloads, or
unverified hardware features.