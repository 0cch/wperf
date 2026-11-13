# Profiling correctness and performance changes

The report at `artifacts/review/assessment.md` evaluated commit `03ddd1f`.
This change addresses the concrete correctness and performance findings:

This is a partial hardening pass, not completion of every recommendation.
The [assessment completion audit](ASSESSMENT_STATUS.md) maps all 14 original
findings, test gaps and roadmap items to their current implementation status.

| Finding | Change |
|---|---|
| PMC profile was indistinguishable from timer samples | Enable PERF_PMC_PROFILE; decode opcode 47 and source IDs; retain native periods; validate requested events after capture |
| Misleading cross-CPU counter deltas | Remove those estimates from stat; report target/source-specific PMC sample distribution |
| Live syscall identity and TID reuse | Shared bounded incremental correlator validates thread lifetimes; invalid TIDs cannot produce durations |
| Samples lose their process when a thread exits | Attribute at sample arrival; timestamp-aware historical fallback for delayed events |
| Shared DbgHelp state in diff | Unique session handles, global DbgHelp API lock and module identity reuse |
| Target starts before capture | Controller waits for READY after the session and guard are configured |
| Sampling configuration and abnormal shutdown | Serialize wperf profile configuration changes; query/restore the selected source; conditional rollback and record/live guards |
| Live LBR has no trigger | Enable profile events and configure the appropriate CPU/PMC trigger |
| Missing loss and capacity accounting | Query ETW loss counters; report metadata/ring/correlation eviction; expire pending records after 60 seconds |
| Cross-process kernel module lookup | Separate kernel fallback from user-space PID lookup, with indexed module address lookup |
| Diff assigns idle gaps to symbols | Use captured sampling periods; PMC time weights stay zero; unknown-sample fallback remains an audit follow-up |
| Wrong-version annotate/archive | Validate PE identity; resolve a matching PDB; archive binaries by hash and PDBs in symbol-store layout |
| Table reports traverse every stack | Only resolve sampled IPs for ordinary rows; build trees or folded stacks only when requested |
| Stack provenance and output escaping | Preserve exact/approximate/missing; constrain fallback to 1 ms and one lifetime; add exact-only output and shared JSON/CSV escaping |

## Interface changes

- CPU sampling uses `-F HZ`. PMC sampling uses `--period N`, measured in the
  source's event counts. Its default is 65536 clamped to the advertised source
  interval range. Combining PMC selection with `-F` is rejected.
- `stat -e SOURCE` selects PMC sampling and isolates the target PID.
- A requested PMC/LBR capture with no corresponding events returns unavailable
  (exit 5); the ETL is retained by record for diagnosis.
- `report --exact-stacks` excludes approximate and missing stacks.
- `report --json --metadata` emits `{ "quality": ..., "rows": ... }`.
  Ordinary `--json` remains an array. `--strict-quality` returns exit 6 on
  ETW loss, buffer/ring loss, metadata eviction, decoder errors or correlator
  eviction. Missing call stacks alone do not imply a corrupt trace.
- `script -F source,period,stack_quality` exposes sample provenance. CPU period
  is in nanoseconds; PMC period is a source-specific event count. Zero means
unavailable. Diff currently falls back to the trace-level interval for a CPU
sample with unknown period; handling late interval metadata is still pending.
- Folded and call-tree frames include module names to keep same-named functions
  in different modules separate.
- Archive binaries are in `modules/<SHA256>/<filename>` and PDBs are in
  `symbols/<PDB filename>/<GUID+age>/<PDB filename>`. The manifests describe
  relative paths. The ETL itself is not copied. Image lookup on another machine
  still requires arranging matching binary paths; archive is not yet a fully
  portable trace-loader format.
- Private-provider capture no longer requires system-profile privilege; the
  provider itself can still restrict capture permissions.

## Validation and remaining boundaries

`wperf_regression_tests` exercises raw sample decoding, anonymous syscalls,
thread reuse, snapshot-boundary correlation, capacity accounting, kernel/user
address spaces, symbol-session isolation, machine output and READY/disconnect
handling without administrator rights. `replay_contract.ps1` checks JSON data
and provenance against the checked-in ETL, as well as same-input diff.

The CPU fixture retains 16104 exact and 496 ambiguous matches. Its former 44
unbounded cross-time stack fallbacks are now missing, bringing missing stacks
to 1219. The ETL bytes have not changed.

The global sampling mutex coordinates wperf processes. External profilers do
not acquire it. PMC setup rejects counters already in use, and rollback checks
the current interval before replacing it. Windows has no matching query for
the full global source list set by TraceProfileSourceConfigInfo; a successful
wperf-owned PMC configuration is cleared when stopped. These policies still
need elevated coexistence/fault tests on target hardware. They do not provide
Linux perf event-group, exact-count or multiplex-scaling semantics.

Real-time sample/event rings and metadata collections are capped at 200000.
Recently ended threads/images are retained for 10 seconds to handle delayed
events. Symbol address caches are capped at 200000; modules unused by the next
live snapshot are unloaded. Offline traces are still materialized in memory;
large-file streaming and stack interning remain future work.

Hardware PMC/LBR behavior, OS/CPU/VBS combinations and sampling perturbation
need an elevated hardware runner. M6 now distinguishes skipped hardware
checks from passed checks and fails advertised PMC sources that produce no
usable events. The soak script records both controller and live worker memory,
handles and CPU time.

New analysis features such as off-CPU flame graphs, scheduler runtime totals,
I/O pathname correlation, JIT maps and full-function annotate are outside this
correctness pass.

## Local report benchmark

Compared the baseline at `03ddd1f` with the optimized Release build using
`tests/data/cpu-stack.etl` (17323 samples), the same symbol search path and
`report --percent-limit 100 --quiet`. Three alternating runs were made after
builds completed. The baseline was built without Zydis; disassembly does not
participate in this report command.

| Build | Three runs (seconds) | Median |
|---|---|---|
| Baseline | 8.894, 9.241, 8.963 | 8.963 s |
| Optimized | 1.113, 0.944, 1.045 | 1.045 s |

This fixture's table-report path is about 8.6 times faster. This measures
offline report execution, including replay and symbol setup; it does not
measure sampling overhead, live latency or large-file peak memory.
