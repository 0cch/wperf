# Hardware Validation Evidence

Updated: 2026-11-16.

This document records Intel hardware validation for LBR, PMC sampling/snapshots,
and AMD64 context registers. Raw traces remain local and are not
committed because they contain machine and process metadata.

## Result summary

| Area | Result | Scope |
|---|---|---|
| Intel LBR | PASS | Timer- and PMC-triggered capture, payload fields, depth, ordering and module ranges |
| PMC sampling and snapshots | PASS | ETW source IDs, periods, per-CPU samples, source switch/restore and CSwitch snapshot semantics |
| AMD64 context registers | PASS | Real payloads, validated field layout, decoding and PMC/LBR coexistence |

PMC values remain ETW sample-derived. This run validates wperf's stated
source/period/snapshot semantics; it does not claim exact Linux
`perf_event_open` counter totals.

## Environment

| Field | Value |
|---|---|
| Machine | Colorful BATTLE-AX B760M-F PRO |
| Firmware | American Megatrends 1010, 2025-05-14 |
| OS | Windows 11 Pro, 10.0.26300, x64 |
| CPU | Intel Core i5-14600K, Family 6 Model 183 Stepping 1 |
| Topology | 14 physical cores, 20 logical processors, one processor group |
| Virtualization | Hypervisor not present |
| VBS | Not enabled |
| HVCI / memory integrity | Not running |
| Privilege | Captures launched through the elevated worker; `SeSystemProfilePrivilege` enabled |
| Build | wperf 0.8.0.0, Release x64 |
| Compiler | MSVC 19.44.35228, Windows SDK 10.0.26100 |
| Base commit | `0fbc66a082d02378cb5ac0b6efa0a76011193036` plus this validation change |

## Workload

`wperf_cpu_burn.exe` from the same Release build was used with its matching PDB.
It executes a deterministic xorshift branch/arithmetic loop and accepts duration
and worker-thread counts. Captures used one or two workers and 1-2 second
windows.

```powershell
.\build-intel\Release\wperf_cpu_burn.exe 2000 2
```
## Intel LBR

The Windows 11 build exposes `TraceConfigureLastBranchRecord`, but both the
callstack and raw configuration calls return `ERROR_NOT_SUPPORTED (50)` on this
CPU. wperf now falls back to the supported legacy sequence:

- `TraceSetInformation(TraceLbrConfigurationInfo)`
- `TraceSetInformation(TraceLbrEventListInfo)` with the PerfInfo timer or PMC
  interrupt hook ID.

Both trigger paths then produced LBR provider events with GUID
`99134383-5248-43fc-834b-529454e75df3`, opcode `0x20`.

| Capture | LBR events | Branch entries | Max depth | Lost events | Decode errors |
|---|---:|---:|---:|---:|---:|
| Timer-triggered, 99 Hz | 510 | 8,194 | 32 | 0 | 0 |
| PMC-triggered, TotalCycles period 1,000,000 | 29,067 | 424,434 | 32 | 0 | 0 |

Field checks:

- Every decoded branch had non-zero from/to addresses.
- The deterministic workload's module range was
  `0x7ff67c710000`-`0x7ff67c71afff`; captured branches contain endpoints in that
  range for both trigger paths.
- PID, TID and CPU are populated from the LBR payload/event context.
- LBR timestamps are populated. If the legacy payload timestamp is zero, the
  reader uses the correlated ETW event timestamp.
- Timestamp order is monotonic per CPU in the validated target subsets.
- Observed payload depth is 1-32 entries and stays below the decoder's 128-entry
  bound.
- `xperf` independently reports the same LBR provider and branch counts.

Reproduction:

```powershell
wperf record --elevate -F 99 --duration 2000 --lbr --no-call-graph `
  -o lbr-timer.etl -- .\build-intel\Release\wperf_cpu_burn.exe 2000 1

wperf record --elevate --pmc-source 19 --period 1000000 --duration 2000 `
  --lbr --no-call-graph -o lbr-pmc.etl -- `
  .\build-intel\Release\wperf_cpu_burn.exe 2000 1

wperf trace -i lbr-pmc.etl -e lbr --json --max 200000
```

## PMC Sampling and Snapshots

| Capture | Source | Period | Samples | CPUs | Lost events | Decode errors |
|---|---:|---:|---:|---:|---:|---:|
| TotalCycles | 19 | 1,000,000 | 46,454 | 20 | 0 | 0 |
| InstructionRetired | 26 | 500,000 | 65,589 | 20 | 0 | 0 |

Both captures reported source IDs and native event-count periods on every
sample. The second capture also proves source switching: source 26 replaced
source 19 and the requested period was preserved. Subsequent captures started
normally after the restore path reset the active profile source to the timer
source.

CSwitch counter snapshots were captured with sources 19 and 26. The PerfInfo
metadata event at opcode `0x31` was decoded to recover the per-CPU source list.
`wperf report --kind pmc --json --metadata` emitted 12,348 rows, all with:

```json
{"exact":false,"semantics":"event-snapshot-derived"}
```

The rows are separated by CPU, source, PID and TID and report deltas plus the
number of contributing intervals. No estimated snapshot was presented as an
exact total.

Reproduction:

```powershell
wperf record --elevate --pmc-source 19 --period 1000000 --duration 2000 `
  --no-call-graph -o pmc-cycles.etl -- `
  .\build-intel\Release\wperf_cpu_burn.exe 2000 2

wperf record --elevate --pmc-source 26 --period 500000 --duration 1000 `
  --no-call-graph -o pmc-instructions.etl -- `
  .\build-intel\Release\wperf_cpu_burn.exe 1000 1

wperf record --elevate --pmc-counters 19,26 --duration 2000 `
  --no-call-graph -o pmc-snapshots.etl -- `
  .\build-intel\Release\wperf_cpu_burn.exe 2000 2

wperf report -i pmc-snapshots.etl --kind pmc --json --metadata
```

All 18 profile sources enumerated by Windows were reported as supported on this
platform; no separate unsupported-source hardware case was available to test.
That distinct inventory item remains open.
## Context Registers

`--context-registers` enabled `TraceContextRegisterInfo` with control and
integer register requests. The resulting trace contains PerfInfo opcode `0x7a`
events on all CPU indexes.

| Capture | CPU samples | LBR events | LBR branches | Context payloads | Lost events |
|---|---:|---:|---:|---:|---:|
| Context only | 918 | 0 | 0 | 740 | 0 |
| PMC + LBR + context | 9,905 | 6,828 | 110,339 | 8,558 | 0 |

Schema sources:

- Microsoft documents the architecture-specific register categories in
  [ETW_CONTEXT_REGISTER_TYPES](https://learn.microsoft.com/en-us/windows/win32/api/evntrace/ne-evntrace-etw_context_register_types).
- The Windows WMI dynamic class `ContextRegistersAMD64` exposes EventType 122
  with `WmiDataId` 1-17 ordered as Rip, Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi,
  Rdi, R8-R15. The class was queried locally with `Get-CimClass`.
- The public [RMP AMD64 context layout](https://github.com/0xrekab/RMP/blob/64cf7112f5bc598b819208fed6a3393ebfdfeab9/rmp/telemetry_sources.h)
  defines the 144-byte payload as a leading QPC timestamp followed by those
  17 registers. The public
  [Windows Kernel Trace event table](https://github.com/jdu2600/Windows10EtwEvents/blob/26394ea7110078f1a19ad68f8db9111f9400142f/mof/Windows_Kernel_Trace.tsv)
  independently lists `PerfInfo_ContextRegistersAMD64_V2` at event type 122.

Validation checks:

- Payload length was 144 bytes for 740/740 standalone events and 8,558/8,558
  combined events.
- The leading QPC timestamp was within 1,000 ticks of the ETW event timestamp
  for 723/740 standalone and 8,396/8,558 combined events.
- RIP was correlated with a sampled instruction pointer in 525/740 standalone
  and 6,606/8,558 combined events. Context logging is not one-to-one with
  sample events, so this is a consistency check rather than an exact pairing rule.
- RSP was 8-byte aligned for 730/740 standalone and 8,451/8,558 combined events.
- The decoder is enabled for AMD64 and exposed through
  `wperf trace -i FILE -e context --json`. ARM64 event type 123 is not decoded
  or claimed.

The combined capture proves that context register logging coexists with PMC
sampling and LBR in one ETL. Field decoding now uses only the validated AMD64
timestamp, RIP, RSP and general-purpose register order.

## Raw evidence

Key ETLs and hashes are retained locally under `evidence/`.

| File | Bytes | SHA256 |
|---|---:|---|
| `lbr-timer.etl` | 13,565,952 | `7D217DE62E3A30C7E6A40B688CB8022DE8B7BE2608E78BA5A7869D1967A07610` |
| `lbr-pmc.etl` | 25,362,432 | `8C76FEBAB68C495B11B019B623CD0000C24050B0E69ADE8E8232CFC56A5D7351` |
| `pmc-cycles-2s.etl` | 14,942,208 | `0BCABB1A0374ABE5E02E5D81BF9E9EC7672227974D71B081111E2BD0D0DD8864` |
| `pmc-instructions.etl` | 14,155,776 | `E8D3959DF37C4DB1B7BA843C2040F653BC1D16E3519C959B41BEB5AB749E108D` |
| `pmc-snapshots.etl` | 21,757,952 | `E98758B2ECDADA3ABE79C3C383710ED867DDFF84D98A9407BA1362843B831F1A` |
| `context-registers.etl` | 13,500,416 | `EAED9829CEF84E4E708B231EC44A0FD3364CCC42F0E3B1D99A10E7640D3EEF1E` |
| `pmc-lbr-context.etl` | 16,252,928 | `D11A391D52A5D852D0BB3BD4944F3DF90D25890A4FDBD783D0ACAC74E91581E3` |

## Test status

Release CTest: 45/45 passed (2 live tests skipped at normal integrity).
The same live tests passed when rerun explicitly through the elevated path:

```text
live_integration          Passed
live_top_controller_smoke Passed
```

## CLI11 and symbol-server regression run

A fresh Intel regression run was executed on 2026-11-14 after the CLI11
command-catalog refactor and the DbgHelp symbol-server loader fix.

| Check | Result |
|---|---|
| Release CTest | 48/48 passed under an elevated shell; live ETW tests included |
| No-Zydis CTest | 48/48 passed at normal integrity; two live tests skipped as expected |
| LBR payload smoke | 1,000 checked events, all with non-zero source/target addresses |
| Context payload smoke | 1,000 checked events, all with non-zero RIP and RSP |
| PMC snapshots | 6,044 rows, all marked estimated and `event-snapshot-derived` |
| Real off-CPU trace | 14,056 blocked and 18,755 ready intervals; 0 lost events and 0 decode errors |
| Public symbols | `ntdll!RtlUserThreadStart`, `kernel32!BaseThreadInitThunk`, and `wperf_cpu_burn!Burn` resolved |
| Symbol cache | Public PDBs downloaded through `_NT_SYMBOL_PATH` into `D:\Symbols` |

The run also validated live scheduler trace, live top, normal and PMC stat,
Zydis annotation, doctor bundle generation, stale-session recovery, and argument
forwarding. Raw ETLs and logs remain under the local `build/validation-*`
directory and are not committed because they contain machine/process metadata.

## Conclusion

LBR, PMC sampling/snapshots and AMD64 context-register decoding are accepted
build. The context-register decoder uses the WMI event type 122 field order and
the independently published 144-byte AMD64 layout, and is exposed through
`wperf trace -e context`. ARM64 context-register payload decoding is not claimed.

## Priority-fix regression validation — 2026-11-16

Host: AMD Ryzen 7 8700G, Windows 11 x64 build 26200. The working tree is based
on 0.8.2.0 with the unreleased priority fixes; builds used MSVC and warnings as
errors, both with and without Zydis.

| Check | Result |
|---|---|
| Non-live CTest, Zydis enabled | 58/58 passed |
| Non-live CTest, Zydis disabled | 58/58 passed |
| Elevated live CTest, each build | 4/4 passed |
| Record worker termination | Independent recovery checkpoint survived controller writes; timer interval restored after guard cleanup |
| PMC interrupt sampling | Real TotalCycles JSON retained sampling source and estimated semantics |
| Single-source CSwitch PMC snapshots | Real counter payloads/source metadata, CPU/time filtering, CSV, and strict-quality exits validated |
| Streaming report and period weights | Final quality matched 17,323 fixture samples; unequal 1/9 ms periods yielded 10/90% in deterministic regression |
| Module identity and hashing | Recorded/local identities separated; hashes cached per local path and enabled with --hash |

On the checked-in CPU fixture and this host's available module files,
`buildid-list --json` took 1.80 s after the fix, versus 78.84 s in the original
evaluation. The default now omits full-file hashing. Explicit `--hash --json`
took 13.79 s with unique-path caching; these timings describe different requested
work and are not an identical-work hashing benchmark. The existing 30/60-second
CTest deadlines were retained.

Scope of this run: CPU, scheduler, PMC sampling/snapshots, output contracts and
cleanup on this AMD/Windows combination. The Intel LBR/context evidence above
remains a separate hardware validation run.

## Global kernel image identity regression — 2026-11-16

The modern image decoder now preserves payload PID 0 instead of substituting
the emitting collector PID. Validation on the same AMD/Windows host:

- Both Release builds (Zydis on/off): 59 non-live tests and 5 elevated live tests
  passed, 64/64 per build, with warnings as errors.
- The new deterministic test failed on the prior decoder, then passed after the
  fix. It covers 32/64-bit payloads, versions 1–3, load/rundown/unload records,
  version-0 and unavailable-PID fallbacks, malformed lengths, cross-process user
  isolation, offline report lookup, live snapshots, and collector termination.
- Fresh live ETL capture verifies global kernel mapping PIDs and kernel CPU
  report attribution to the recorded ntoskrnl image without a diagnostic overlay.
- Five existing self-profile ETLs were replayed by the normal CLI with matching
  local kernel symbols. Target sample counts were unchanged (669, 644, 1900,
  922, 2541); unknown-module overhead fell from 60–82% to 0–0.053%.

Original ETLs and local replay outputs remain under the ignored `out/` tree;
no machine-specific trace data is included in these regressions.

## Interrupt, heap/pool, and driver diagnostics (2026-11-16)

Validation used AMD Ryzen 7 8700G / Windows 11 build 26200, MSVC Release x64,
warnings as errors, and both Zydis-enabled and disabled configurations. Each
configuration passed 61 non-live tests and all six elevated integration tests
(67/67, no skips). The live diagnostic test uses the dedicated DPC/ISR profiles,
native HeapAlloc/HeapReAlloc/HeapFree with 64 KiB retained allocations, the
`File` pool tag, and uncached/write-through I/O for driver calls. Capture loss,
unsupported layouts, and decode errors are rejected, not accepted as success.

Raw-payload regressions cover both pointer widths, truncation, unsupported
versions, inclusive/union time, time clipping, address reuse, heap/process
termination, native realloc notifications following their internal alloc/free
events, driver unique-ID mismatches, and missing context. Start/end rundown
boundary regressions restore pre-existing metadata without inventing creation,
exit, or unload times. The unchanged scheduler fixture now has 363 unknown-PID
correlations rather than the former 10,623; event/sample counts and ETL bytes
remain unchanged.

An independent xperf `tracestats -detail storage` pass on the same kernel ETL
reported 11,269 DPC + 239 timer-DPC events, 5,493 message-signaled + 743 other ISR
events, and 834,206 driver dispatch entries / 834,198 returns. wperf reports
11,508 DPC and 6,236 ISR executions with zero unknown module rows, plus 834,198
paired dispatch spans and eight unclosed entries. Inclusive execution/call spans
and clock-resolution zero-duration observations retain their documented meanings;
they are not exclusive CPU time or decomposed minifilter/device service time.

Initial unfiltered pool experiments enabled unrelated memory/page-list traffic
and caused excessive trace volume/loss. The implemented pool profile uses only
the pool group, a required tag filter, compact scheduling context, and selective
replay. Kernel/driver/pool captures use bounded 128 KiB buffers, 32 minimum and
256 maximum. A high-rate kernel capture that lost events was rejected; the
dedicated profiles and final test runs passed the loss gates.

Heap module attribution uses a controller-side attach snapshot of loaded PE
headers, with explicit snapshot provenance; dynamic DLL lifecycle completeness
is not claimed. Allocation stacks can be missing and remain counted. Unmatched
pool frees reflect the unknown pre-capture baseline, and observed live bytes do
not prove a leak. Raw ETLs, xperf output, and test logs remain in ignored `out/`.
