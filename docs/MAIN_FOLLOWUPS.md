# Main-branch profiling follow-ups

This pass follows the assessment audit at `b050f07`. Work is committed directly
to main; the merged profiling-hardening branches have been removed.

The consolidated remaining-work checklist, dependencies and acceptance criteria
are maintained in [TODO.md](TODO.md).

## Implemented and covered by regressions

- Unknown per-sample periods stay zero even when later metadata provides an
  interval. Diff adds sample shares, percentage-point differences, trace spans,
  known-period coverage and an approximate independent-binomial margin. These
  are sampling statistics, not exact execution time or performance causality.
- CPU aggregation uses process lifetimes plus full recorded module identity
  and function RVA. Folded/tree labels preserve instance and module identity.
  Tests cover Unicode, same-named DLL versions, PID reuse and late metadata.
- Raw scheduler/syscall/pagefault/file/disk decoding is shared by live and
  offline consumers. Short known payloads fail instead of manufacturing zeros.
  Disk init and flush schemas are distinct; issuer IDs and file keys are used
  where provided. File names and operation metadata survive completion pairing.
- StackWalk keys resolve within delete/rundown epochs rather than using the
  last definition forever. Identical frame arrays are interned. Regression
  inputs cover modern refs/defs and key reuse; `record --stack-cache` enables
  native ETW caching when supported.
- DbgHelp expands inline frames from optimized PDBs. The bounded, versioned
  `.symcache/<identity>/symbols.v1` cache includes binary/PDB identity, RVA,
  source and inline context. A generated optimized PDB fixture verifies inline
  expansion and cache reopening; stale PE/PDB identity remains rejected.
- `report/script/annotate/diff --archive DIR` validates `modules.tsv`, PE
  identity and SHA256 before mapping unavailable original binary paths.
  Traversal/outside-directory and modified binaries are rejected. Matching
  PDBs are stored beside binaries and in symbol-store layout. Archive can
  include its ETL with `--include-trace`.
- `report --stream` performs metadata and sample passes and retains no sample
  or event windows. It supports CPU tables/JSON/CSV and omits stacks; combining
  it with call graphs, folded or exact-stack output is rejected. Ordinary
  replay has a conservative 512 MiB managed allocation budget; aggregation has
  a 200000-row limit.
- Live snapshots drain lifecycle changes under the callback lock and apply,
  copy and prune metadata outside it. Window buffers are budgeted at 128 MiB,
  metadata at 64 MiB and symbol caches at 64 MiB total (32 MiB each). These are
  managed-data budgets, not a promise about DbgHelp/kernel or total process RSS.
- Live/record workers keep a controller handle and a five-second teardown
  watchdog. It terminates only its own authenticated worker process if cleanup
  hangs; it never detaches a thread that still references destroyed state.
  The session guard is launched before capture configuration, with recovery
  checkpoints persisted before changing global sampling settings.
- Live LBR also rejects successful empty captures. Final loss is reported;
  report quality gates support `--max-lost-events` and `--max-missing-stacks`.
- Scheduler output adds observed running/ready/blocked intervals and wakeup
  P50/P95/P99. Boundary fragments without both endpoints are not counted.
  I/O output adds per-path/operation byte totals and latency quantiles.
- Offline reports add `--include-children`, `--cpu`, `--time-start`,
  `--time-end` (milliseconds relative to capture), and `--scope all|user|kernel`.
  The off-CPU path crops intervals at relative-time bounds and honors the same
  PID/descendant, CPU, stack, module, symbol and scope selectors.
- Off-CPU reports expose confirmed and truncated-boundary durations separately.
  Truncated intervals are excluded by default; `--off-cpu-include-inferred`
  opts them in, and strict quality rejects them. Lost-event and missing-stack
  gates are shared with CPU reports.
- Live top decodes and drains control IPC independently from bounded rendering.
  Oversized metadata/stack payloads are truncated at safe boundaries and counted
  in `metadata_dropped`; worker sends check STOP between messages.
- `annotate --disasm` decodes a known function range with per-instruction sample
  hits; symbols without a known size retain the bounded address-heat fallback.

## PMC capture modes

```powershell
wperf record --elevate --pmc-source ID --period COUNT -o sample.etl -- app.exe
wperf record --elevate --pmc-counters ID1,ID2 --profile sched -o counters.etl -- app.exe
wperf report -i counters.etl --kind pmc --json
```

Sampling and counter snapshots require separate captures. Snapshots configure
`TracePmcCounterListInfo` and `TracePmcEventListInfo` on CSwitch. Source ordering
and names are emitted into the capture. Differences are computed only within
one CPU/source stream and attributed to the old thread where known; decreases
are not interpreted as wrap/reset totals. Interrupt activity can be included
in such intervals. Output explicitly declares `exact:false` and
`event-snapshot-derived`; no perf-event groups or multiplex scaling is claimed.
Mixed-source merged traces and unknown source metadata are not inferred.

## Validation and remaining hardware boundaries

`wperf_live_integration_tests` requires real CPU samples and decoded live sched
events, graceful STOP, worker-kill cleanup and controller-kill cleanup with no
matching output. A separate live smoke runs the ordinary `wperf top` controller
through decode, symbolization and render. Ordinary local sessions skip with
exit 77 when not elevated. CI sets `WPERF_REQUIRE_LIVE_TESTS=1`, so access denial
or empty capture fails.
The portable regression also tests the teardown watchdog, raw payload mutation,
streaming a real checked-in ETL, inline/cache and verified archive replay.

Issue #3's decoder/cache/inline regressions and Issue #4's mandatory elevated
integration passed in both CI configurations at `10825e8` (37/37 each):
https://github.com/0cch/wperf/actions/runs/43410931662 . Both issues are closed.
Issue #5 still requires supported Intel LBR, PMC calibration and context-register
integration on target hardware; implementing configuration and decoders does
not satisfy those acceptance conditions by itself.

Windows has no supported query for the previous global PMC source list.
Conditional interval rollback and ownership rejection cannot provide atomic
coexistence with unrelated profilers. Complete cross-OS/CPU/VBS/WOW64 coverage,
long soak and sampling perturbation measurements remain open. Context-register
payload decoding remains disabled until its schema is validated. Off-CPU
blocked/ready stack analysis is now implemented for the offline record/report
path; JIT maps and live off-CPU remain separate analysis work.
