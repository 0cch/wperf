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
core plumbing. Review-4392026 then found CSwitch attribution, off-CPU
aggregation/boundary/filter/gate, and live IPC progress defects. Those fixes are
implemented in the current tree and await the next acceptance review.

## Resolved findings

- Sample periods are preserved explicitly and are not backfilled by later metadata.
- Module-version identity participates in aggregation keys and cache identity.
- Stack references are lifetime-aware and bounded.
- Inline frames survive cache reopen and identity validation.
- Live top symbolization runs in the ordinary-permission controller, not the elevated worker.
- Scheduler stack capture and off-CPU blocked/ready correlation are implemented.
- CSwitch now treats `tid` as incoming and `related_tid` as outgoing throughout scheduler-stack and off-CPU correlation.
- Off-CPU reports preserve inferred/truncated boundaries, crop relative-time windows, and apply common quality gates.
- Live IPC payloads are bounded and controller rendering no longer blocks the receive/control loop.
- Repeated milestone smoke scripts were removed; focused CTest coverage is authoritative.

## Remaining validation gaps

| Area | Remaining work |
|---|---|
| Intel LBR | Validated on Intel Core i5-14600K: timer and PMC captures decode non-empty branches; see [HARDWARE_VALIDATION.md](HARDWARE_VALIDATION.md) |
| PMC calibration | ETW source IDs, periods, per-CPU deltas, restore and `exact:false` snapshots validated; unsupported-source inventory remains open |
| Context registers | AMD64 capture and decoding validated against the WMI event type 122 field order and public RMP layout; ARM64 not claimed |
| Platform matrix | OS/CPU/VBS/HVCI/WOW64/processor-group coverage |
| Real cached stacks | Native StackWalkDef/Ref ETL evidence |
| Soak and perturbation | Long runs, fault injection, overhead/loss curves |
| JIT | Runtime-specific method lifecycle mapping |
| External profilers | Controlled conflict and recovery testing |

## Current verification

- Stable CTest coverage includes CLI contracts, replay, archive, regression,
  folded output, off-CPU output/quality-gate behavior, full top controller
  decode/render smoke, and live integration.
- CI runs both Zydis-enabled and Zydis-disabled Release builds.
- CI sets `WPERF_REQUIRE_LIVE_TESTS=1`, so an unexpected live-test skip fails.
- LBR, ETW PMC semantics and AMD64 context-register decoding now have Intel hardware evidence.

The project does not claim exact Linux perf equivalence for event groups,
multiplex scaling, syscall argument semantics, ARM64 context-register
payloads, or unverified hardware features.
