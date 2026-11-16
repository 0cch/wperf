# Compatibility

wperf targets Windows ETW capabilities. Support is capability-based: a command
can be valid on Windows but unavailable on a particular CPU, Windows build, or
device configuration.

## Platform matrix

| Area | Status |
|---|---|
| Windows 11 x64 | Supported |
| Windows 10 21H2+ x64 | Supported target; broader matrix still being expanded |
| WOW64 / 32-bit ETL replay | Must not crash; full symbol coverage is not claimed |
| Multiple processor groups | Code handles processor indexes, but broad hardware validation is pending |
| VBS/HVCI off | Validated on the current Intel test system |
| VBS/HVCI on | Validation pending |
| Elevated system ETW capture | Supported |
| Normal-integrity offline replay | Supported |
| Custom kernel driver / direct MSR access | Not supported by design |

## Toolchain

| Component | Requirement |
|---|---|
| Compiler | Visual Studio 2022 C++ |
| CMake | 3.24+ |
| Windows SDK | Windows 10/11 SDK |
| Runtime | Windows system DLLs only; no WPA/WPR dependency |
| Optional disassembly | Zydis/Zycore supplied by the vcpkg manifest, or build with disassembly off |

## Feature compatibility

| Feature | Support and limits |
|---|---|
| CPU sampling | ETW system logger; sample weights are nanosecond periods |
| Call stacks | Direct StackWalk and modern StackWalkRef/Def where supported |
| Symbols | DbgHelp, local PDBs, optional symbol-server configuration, identity-aware symcache |
| Scheduler | CSwitch/ReadyThread; scheduler stacks requested for sched profiles |
| Syscall | Enter/exit duration only when identity is trustworthy; arguments are not synthesized |
| Page faults | Classic ETW page-fault events |
| File/disk I/O | Init/completion correlation by IRP and file identity where available |
| Off-CPU | Blocked/ready intervals with truncated-boundary and wakeup-source reporting |
| DPC/ISR | Classic PerfInfo execution spans; system ownership and inclusive/union time |
| Native heap | PID-scoped ETW allocation lifetimes after attachment; no pre-capture baseline or managed/custom allocator coverage |
| Kernel pool | Pool tag/type/session lifetimes; requires a tag filter; system ownership; observed-unfreed does not prove a leak |
| Driver calls | DiskIo dispatch/complete-request call spans with labeled scheduler context; no inferred minifilter or device service-time split |
| PMC interrupts | Windows ETW profile sources; source-specific periods |
| CSwitch snapshots | Estimated per-CPU/old-thread deltas; always `exact:false` |
| Intel LBR | Supported on capable Intel systems; depth is finite and not Intel PT |
| Context registers | AMD64 payload decoding validated; ARM64 decoding not claimed |
| Private providers | Raw ETL capture; arbitrary TDH schema rendering not implemented |
| Archives | SHA256 module/PDB identity and symbol-store layout |
| Disassembly | Optional Zydis, Intel syntax |

## Hardware validation status

Intel Core i5-14600K / Windows 11 build 26300 was used for:

- timer- and PMC-triggered LBR;
- TotalCycles and InstructionRetired PMC sampling;
- CSwitch PMC snapshots;
- AMD64 context registers;
- combined PMC + LBR + context capture.

See [HARDWARE_VALIDATION.md](HARDWARE_VALIDATION.md) for counts, commands,
hashes, and limitations.

AMD Ryzen hardware has been used for general ETW/CI validation, but Intel-only
features are not claimed to pass there. ARM64 context-register payload decoding
has not been implemented.

## Capture and output semantics

- `record -p PID` is system-wide ETW capture followed by analysis filtering; it is
  not a kernel-level per-process attach.
- Without `--duration`, system capture runs until Ctrl+C. A target command stops
  capture when its root process exits.
- `trace --max 0` means unlimited output. A finite `--max` emits a truncation
  warning, and `trace --json --metadata` reports `truncated` explicitly.
- `stat` JSON distinguishes Windows process API counters (`windows-api`) from
  event-sampled ETW PMC data (`etw-pmc-sampling`).
- `diff --normalize weight|samples` changes the denominator used for share
  comparisons; incompatible sample sources are rejected. Symbol matching is
  the default; `--match identity` restores strict build/RVA matching.
- CPU/PMC sample reports default to period weights with explicit count fallback
  for unknown periods; `--weight samples` selects sample counts.
- `buildid-list` preserves recorded PE metadata, labels local mismatches, and
  computes local-file hashes only with `--hash`.
- Thread stat counters are observation deltas with 50 ms discovery; observed
  exited threads are retained, while shorter unobserved threads may be missed.

## Semantics that differ from Linux perf

- ETW does not expose Linux `perf_event_open` event groups or exact multiplexing
  semantics.
- PMC results are sample-derived unless explicitly documented otherwise.
- Kernel/user address attribution depends on ETW metadata and symbol identity.
- Modern image payload PID 0 remains global; kernel modules can be looked up
  from any target PID. Version-0 image records use header identity because their
  payload contains no PID. User-module mappings remain process-specific.
- Syscall arguments and kernel-internal memory are not reconstructed.
- LBR is a finite branch history, not Processor Trace.
- Windows sampling configuration is machine-global; unrelated ETW sessions can
  affect or be affected by source/interval ownership.

## Output compatibility

- JSON/CSV field names and exit codes are intended for automation.
- New fields may be added in a backward-compatible way; existing meanings should
  not silently change.
- Missing, ambiguous, unsupported, and estimated data must remain identifiable.
- A skipped or unsupported hardware test is not a pass.

## Known limitations

- Full Windows 10/11, VBS/HVCI, WOW64, and multi-group hardware matrices remain
  open.
- Real scheduler-stack and cached-stack evidence is not yet complete for every
  Windows build.
- Long soak, fault injection, and perturbation curves remain manual work.
- JIT method lifecycle mapping is not implemented.
- External-profiler coexistence cannot be made fully atomic because Windows
  does not expose a complete query for prior global PMC source lists.
