# Interrupt, memory, and driver diagnostics

These workflows use native Windows ETW. They do not require xperf/WPR at runtime
or a custom driver. Capture requires the elevated worker; replay, symbols,
aggregation, and file output remain in the normal-integrity controller.

## Capture and report

```powershell
# DPC and ISR separately, or analyze both from an existing kernel trace.
wperf record --elevate --profile dpc --duration 2000 -o dpc.etl
wperf report -i dpc.etl --kind dpc --json --metadata
wperf record --elevate --profile isr --duration 2000 -o isr.etl
wperf report -i isr.etl --kind isr --csv
wperf report -i kernel.etl --kind interrupt --symbol-path .\symbols

# Attach to an existing native process; pre-attachment allocations are unknown.
wperf record --elevate --profile heap -p 1234 --duration 2000 -o heap.etl
wperf report -i heap.etl --kind heap -p 1234 --json --metadata -o heap.json

# System pool allocation/free lifetimes, grouped by tag/type/session and stack.
wperf list --pool-tags --max 20
wperf record --elevate --profile pool --pool-tag File --duration 1000 -o pool.etl
wperf report -i pool.etl --kind pool --json --metadata

# Observable driver dispatch and complete-request call spans.
wperf record --elevate --profile driver --duration 1000 -o driver.etl
wperf report -i driver.etl --kind driver --symbol-path .\symbols --csv
```

All diagnostic report kinds support text, JSON, CSV, `-o`, symbol paths, verified
archives, module/symbol filters, capture-relative time ranges, `--cpu`, and
loss/strict-quality gates. Heap and driver also support PID/TID/comm/children
selection. DPC/ISR and pool reject these process selectors because execution
context does not establish ownership. Heap/pool support `--exact-stacks` and
`--max-missing-stacks`; execution reports use recorded routine addresses and do
not require event stacks. Empty selections exit 5; failed quality gates exit 6.

Sampling weights/source/scope/percent/sort, call trees, folded output, off-CPU,
and streaming options are not applicable to these reports and are rejected.
`trace`, `top`, and `diff` retain their existing supported domains; these new
domains are exposed through `report --kind`.

The five dedicated capture profiles do not enable CPU sampling and reject `-F`,
PMC, LBR, context registers, stack caching, and private-provider combinations.
Heap requires an existing nonzero PID and does not launch a new target. It uses
an ETW heap-session PID extension, without persistent IFEO registry changes.
Pool requires a four-character `--pool-tag` to focus its capture/stack filter
and analysis; `list --pool-tags --json` exposes current system tag counters.
Allocation/free snapshot counters can wrap and are not leak proofs.
Pool/driver tracing can produce high event rates and substantially perturb a
workload; start with short, dedicated captures. They are not enabled by `all`.
`kernel` retains its CPU/DPC/ISR/driver event configuration.

## DPC and ISR interpretation

Supported classic PerfInfo records contain an initial QPC timestamp and routine
address; the event timestamp is the end. DPC, timer DPC, threaded DPC, and ISR
remain separate row kinds. Rows group by processor, subtype, routine, and image
lifetime. Kernel symbols use global image mappings. JSON ownership is `system`,
and PID/TID are null rather than copied from the interrupted thread.

`inclusive_ns` sums the selected execution spans; `busy_ns` is their interval
union within a row. Maximum and p50/p95/p99 use individual selected spans.
Time selection clips spans at both window boundaries. Nested ISR/DPC work can
overlap; summing rows does not yield exclusive CPU time. No causal claim about
a particular process stall is inferred from temporal proximity alone.

## Heap and pool interpretation

Allocation/free/reallocation records are replayed in time order. Heap keys
include process instance, heap handle, and address. Pool keys are system-wide
and distinguish session pool addresses. Address reuse with a missing free is
counted and marks the analysis incomplete. Realloc releases the old identity
before recording the new allocation. Native alloc/free records preceding a
realloc notification are consumed once, and unchanged realloc notifications do
not invent allocation churn. Notifications are counted separately in quality.
Heap destruction and observed process
exit close that heap lifetime, with bytes reported separately from explicit
free events.

Address reuse that lacks a free invalidates the prior observed allocation;
its bytes are recorded as `invalidated_bytes`, not credited as a confirmed free
or lifecycle release.

Rows group by allocation stack and heap identity, or pool tag/type/session.
The representative module/symbol comes from the first recorded stack frame
outside common heap/kernel wrappers, falling back to the first frame. This is
an allocation site, not proof that a named driver owns or leaks a pool tag.
The full recorded stack is present in JSON. Pool capture also includes CSwitch
to recover the execution TID for exact-timestamp stack lookup when short event
headers omit it; this does not establish process ownership. Missing stacks retain an unknown
site and are counted; exact stack correlation never uses a nearby event guess.

`allocated_bytes`, `freed_bytes`, `live_bytes`, and `peak_live_bytes` describe
allocations observed in the capture. A free without an observed allocation
increments `unmatched_frees`; it does not create a fictitious baseline allocation
or a negative live count. Outstanding allocations are labeled `observed-unfreed`,
not leaks. Event loss can make these totals either too high or too low.

PID/TID/CPU/time-start filtering selects allocation cohorts. All relevant frees,
reallocations, and lifecycle exits up to `--time-end` are still consumed, even
when performed by another thread or CPU. Thus a selected cohort's live bytes
are evaluated at the selected end, and its peak covers that cohort's observed
lifetime. Allocations predating the selected start are not added to the cohort.
Module/symbol filters apply after reconstruction. Heap events do not cover
arbitrary custom allocators, VirtualAlloc-only memory, or managed GC objects.

Native heap captures include a controller-written process/module attach
snapshot. Snapshot mappings are labeled `module_provenance: "attach-snapshot"`
in memory rows and stack frames; `quality.heap_module_lifecycle` reports this
coverage. They are not complete module load/unload histories: mid-capture DLL
changes can be unresolved or make snapshot-derived attribution uncertain.
ETLs with real image lifecycle events use `etw` provenance; missing mappings
use `unavailable`. Module/PDB identity checks remain required. Snapshot PE
identity comes from loaded PE headers in the target process; a possibly replaced
local file is not used as capture identity. This inspection occurs only in the
ordinary controller.

## Driver interpretation

DiskIo driver major-function call/return and complete-request call/return pairs
are correlated by IRP, unique match ID, and operation family. Identity mismatch,
duplicate entries, and unpaired capture boundaries are exposed in quality;
no fabricated duration is emitted. Completion-routine point events are counted
separately and never converted into durations without a matching boundary.

Driver capture includes CSwitch events. Rows use header or thread-lifecycle
context where supplied, otherwise same-CPU scheduling establishes entry context.
`context_basis` distinguishes `event-header`, `thread-lifecycle`, `scheduler`,
and `unavailable`; missing PID/TID remain null. Routine
identity, major/minor function numbers, processor, count, inclusive/union time,
maximum, and quantiles describe `observed-call-span`. Processor means the entry
processor; the call may include nested work, waits, or migration. The union is
an elapsed-span union, not CPU running time. A driver's own execution cost,
device service time, IRP end-to-end latency, and per-minifilter latency cannot
be recovered by subtracting these rows.

Extra stack walking is intentionally not requested for driver calls: their
routine addresses already support module/PDB resolution, and per-call stacks
can amplify ETL writer I/O.

## Quality, budgets, and validation

Diagnostic replay stores scheduling context compactly, retains lifecycle metadata,
and keeps only the selected event
domain (plus allocation stacks where required). Unrelated samples/events do
not consume its retained-event budget. The 512 MiB replay budget still applies;
analysis limits pending lifetimes/pairs to 200,000 and rows to 100,000, reporting
dropped state as incomplete rather than silently presenting exact totals.

`--json --metadata` adds `quality.analysis` with baseline semantics, unmatched
frees, address reuse, unpaired/duplicate driver calls, completion points, budget
drops, and missing allocation stacks. Unsupported diagnostic event versions are
counted in `diagnostic_unsupported` and affect `incomplete`. Delivery/decode
quality does not guarantee symbol coverage or a complete pre-capture baseline.

The JSON shapes are in `diagnostic-execution.schema.json` and
`diagnostic-memory.schema.json`. Decoder/analysis tests cover 32/64-bit payloads,
truncation, unsupported versions, overlap, cropping, lifecycle identity, and
correlation failures. Schema/CLI tests cover structured output, empty results,
and incompatible options. Elevated `live_diagnostics_smoke` exercises real
interrupts, native heap realloc/free with retained allocations, system pool,
and uncached/write-through I/O. xperf comparison is an additional developer
validation step, not a runtime dependency.
