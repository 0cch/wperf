# wperf

**A lightweight Windows profiler built on ETW — closer to the `perf` workflow,
without a custom driver.**

[![CI](https://github.com/0cch/wperf/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/0cch/wperf/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/0cch/wperf?sort=semver)](https://github.com/0cch/wperf/releases)
[![License: MIT](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Windows%2010%2F11-blue.svg)](docs/COMPATIBILITY.md)

`wperf` captures CPU samples, call stacks, scheduler events, page faults, I/O,
off-CPU time, PMC samples, Intel LBR branches, and AMD64 context registers. It
then gives you perf-style tables, call trees, JSON/CSV, folded stacks, and
self-contained interactive HTML FlameGraphs — with explicit quality,
symbolization, and compatibility metadata.

> Pre-1.0: useful today, but CLI and machine-readable fields can still evolve.
> Compatibility rules are documented in [PUBLIC_CONTRACT.md](docs/PUBLIC_CONTRACT.md).

## Why wperf?

WPA/WPR and PerfView are excellent full-featured tools. wperf focuses on the
developer loop:

- **Fast command line:** record, report, top, trace, diff, annotate.
- **Scriptable output:** deterministic exits, JSON schemas, CSV, folded stacks.
- **Agent surface:** capability discovery, structured errors, and `record --json`.
- **Clear semantics:** missing, approximate, lost, and estimated data stay visible.
- **Small trust boundary:** only a hidden worker runs elevated.
- **No kernel component:** ETW only; no driver, patching, or PMU MSR access.
- **Windows-native:** PDBs, ETW stacks, process/thread lifetimes, and ETW quality counters.

## What can it do?

| Workflow | Commands |
|---|---|
| Find CPU hotspots | `record`, `report`, `top` |
| Inspect call trees and FlameGraphs | `report --call-graph`, `report --folded`, `report --flamegraph`, `flamegraph` |
| Diagnose stalls and scheduling | `record --profile sched`, `report --off-cpu`, `trace -e sched` |
| Inspect syscalls, faults, and I/O | `trace -e syscall`, `trace -e pagefault`, `trace -e io` |
| Diagnose interrupts, native allocations, and driver calls | `report --kind dpc\|isr\|heap\|pool\|driver` |
| Hardware profiling | PMC intervals, CSwitch snapshots, Intel LBR, AMD64 context registers |
| Compare before/after | `diff --calltree` |
| Reproduce symbols later | `archive`, `buildid-list`, `.symcache` |
| Inspect instructions | `annotate --disasm` with optional Zydis |

## 60-second tour

```powershell
# 1. Build
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static-md `
  -DVCPKG_MANIFEST_MODE=ON
cmake --build build --config Release --parallel

# 2. Capture your app
.\build\Release\wperf.exe record --elevate --duration 5000 `
  -o app.etl -- .\path\to\app.exe

# 3. Show hot functions and call stacks
.\build\Release\wperf.exe report -i app.etl `
  --symbol-path .\build\Release --call-graph
```

Discover the full public surface with `wperf help`,
`wperf help <command>`, or the generated
[command reference](docs/COMMAND_REFERENCE.md). Perf-style forms such as
`-F99`, `--percent-limit=1`, and `-- command args` are supported.

Illustrative report shape:

```text
Overhead  Samples  Command     Module       Symbol
  42.1%     1840   app.exe     app.exe      render_frame
  17.4%      762   app.exe     app.exe      decode_packet
  11.8%      516   ntdll.dll   ntdll.dll    RtlAllocateHeap
```

Generate a self-contained interactive flame graph:

```powershell
.\build\Release\wperf.exe report -i app.etl `
  --symbol-path .\build\Release --flamegraph --quiet -o app.html
```

The HTML contains inline CSS and JavaScript and does not require Perl, a network
connection, or an external FlameGraph repository. The standalone converter can
also render an existing folded file:

```powershell
.\build\Release\wperf.exe flamegraph -i stacks.folded `
  --countname nanoseconds -o stacks.html
```

The external `flamegraph.pl` SVG workflow remains supported. For sample-count
weights:

```powershell
.\build\Release\wperf.exe report -i app.etl `
  --symbol-path .\build\Release --weight samples --folded --quiet `
  -o stacks.folded
perl flamegraph.pl stacks.folded > stacks.svg
```

For period weights, pass the matching `--countname nanoseconds` for CPU or
`--countname events` for PMC to `flamegraph.pl`, and check for the documented
unknown-period fallback.

CPU tables, call trees and folded stacks default to sampling-period weights
(CPU nanoseconds or PMC event counts). Use `--weight samples` for counts;
unknown periods fall back to counts with a diagnostic. `--source ID` selects a
single source when an ETL contains multiple sample domains.

`diff --match symbol` aligns resolved module/function names across builds;
`--match identity` requires strict build identity. `buildid-list` distinguishes
recorded and local module identity; use `--hash` for cached local-file SHA256.

`report --verbose` also prints the DbgHelp and `.symcache` paths, cache-layer
hit/miss counts, DbgHelp work, unresolved addresses, and per-module rejection or
missing-PDB details. `report --json --metadata` exposes the same diagnostics
under `symbols`, including full local paths for troubleshooting.

`--symbol-path` augments rather than replaces `_NT_SYMBOL_PATH`. Repeat the
option or use a semicolon-separated value to search multiple local PDB
directories before the configured symbol server.

Generated HTML FlameGraphs support click-to-inspect frame details, stack copying,
double-click zoom, drag-select range zoom, searchable frames with Enter /
Shift+Enter navigation, Backspace/Up navigation, Escape reset, and a minimum
percentage filter for decluttering. The current zoomed view can be downloaded
directly as SVG or PNG from the HTML toolbar.

## Common workflows

See [diagnostic workflows](docs/DIAGNOSTICS.md) for DPC/ISR execution spans,
native heap/pool lifetimes, and observable driver call spans. These are dedicated
ETW captures with explicit ownership, boundary, and baseline limitations.

### Live CPU view

```powershell
wperf top --elevate --interval 1000
```

### Off-CPU stalls

```powershell
wperf record --elevate --profile sched --duration 5000 -o sched.etl
wperf report -i sched.etl --off-cpu --symbol-path .\symbols
```

### Machine-readable automation

```powershell
wperf capabilities --json
wperf --json-errors report -i app.etl --json --metadata --strict-quality
wperf trace -i sched.etl -e sched --json --max 100
```

Agent-oriented discovery, error handling, bounded-output rules, and structured
capture results are documented in [AGENT.md](docs/AGENT.md).

### Hardware counters and branch history

```powershell
wperf record --elevate -e TotalCycles --period 1000000 `
  --duration 3000 -o pmc.etl
wperf record --elevate --lbr -F 99 --duration 3000 -o lbr.etl
wperf trace -i captured-with-context.etl -e context --json
```

## Build requirements

- Windows 11 or Windows 10 21H2+ x64.
- Visual Studio 2022 with the Desktop C++ workload.
- Windows SDK 10 and CMake 3.24+.
- Windows SDK Debugging Tools optional for public-symbol downloads from a symbol server.
- PowerShell 7 recommended for development and release scripts.
- vcpkg checkout for manifest mode (CI uses `VCPKG_INSTALLATION_ROOT`).
- Elevation for system capture; offline replay runs at normal integrity.

The release package does not bundle DbgHelp or symsrv. wperf prefers a
compatible `dbghelp.dll` from `WPERF_DBGHELP_PATH`, the application directory,
or Windows SDK Debugging Tools, then falls back to the system copy. With a
symsrv-capable DbgHelp and `_NT_SYMBOL_PATH`, public system symbols can be
downloaded into a local cache. CLI11, nlohmann/json, Zydis, and Zycore are
managed by the repository `vcpkg.json` manifest and consumed as imported CMake
targets. Release and CI builds use `x64-windows-static-md`; disassembly can be
disabled with `-DWPERF_ENABLE_DISASM=OFF`.

## Design principles

- **Evidence over guesswork.** Quality counters and provenance are part of the result.
- **Exact is a claim.** Estimated PMC snapshots are never presented as exact totals.
- **Least privilege.** DbgHelp, PDB parsing, aggregation, and rendering stay in the controller.
- **Stable automation.** Exit codes and documented JSON fields are compatibility surface.
- **Small, extensible core.** One replay model feeds reports, scripts, diffs, and live views.

## Compatibility

| Area | Status |
|---|---|
| Windows 10/11 x64 | Supported; broader hardware matrix ongoing |
| Intel LBR | Hardware-validated on Intel i5-14600K |
| PMC sampling/snapshots | Hardware-validated; values are ETW-derived |
| AMD64 context registers | Capture and decoding validated |
| ARM64 context registers | Not decoded |
| Custom driver / direct MSR | Not supported by design |

See [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md) and the
[hardware validation evidence](docs/HARDWARE_VALIDATION.md).

## Documentation

- [Complete usage guide](docs/USAGE.md)
- [Command reference](docs/COMMAND_REFERENCE.md)
- [Development guide](docs/DEVELOPMENT.md)
- [Architecture and trust boundaries](docs/ARCHITECTURE.md)
- [Agent usage and machine contract](docs/AGENT.md)
- [Public contract and versioning](docs/PUBLIC_CONTRACT.md)
- [JSON schemas](docs/schemas)
- [Compatibility matrix](docs/COMPATIBILITY.md)
- [Roadmap and open work](docs/TODO.md)
- [Release process](docs/RELEASE.md)
- [Contributing](CONTRIBUTING.md)
- [Security policy](SECURITY.md)

## Contributing

Contributions are welcome across ETW capture and decoding, analysis, symbols,
diagnostics, machine-readable contracts, documentation, and hardware validation.
High-value areas include:

- test Windows/CPU/VBS/HVCI/WOW64/multi-group combinations;
- contribute sanitized replay fixtures and decoder boundary tests;
- improve cached/scheduler stacks, symbolization, heap/pool/driver diagnostics,
  profiler coexistence, and sampling overhead evidence;
- extend CLI conformance, JSON Schema, capabilities, structured-error, and Agent
  contract coverage;
- improve examples, troubleshooting, compatibility notes, and hardware evidence;
- implement an item from [docs/TODO.md](docs/TODO.md).

Before opening a pull request:

- read [CONTRIBUTING.md](CONTRIBUTING.md) and
  [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md);
- run the normal CTest suite and relevant elevated/live ETW tests;
- regenerate `docs/COMMAND_REFERENCE.md` after changing the command catalog;
- update schemas, public-contract/Agent docs, diagnostics, and hardware evidence
  when the affected surface changes;
- do not include raw traces, dumps, credentials, user names, host names, or
  private machine paths in public issues or pull requests.

See [docs/AGENT.md](docs/AGENT.md) for the Agent-facing contract and
[docs/PUBLIC_CONTRACT.md](docs/PUBLIC_CONTRACT.md) for compatibility rules. For
vulnerabilities, use the private process in [SECURITY.md](SECURITY.md).

If wperf saves you time, a star helps other Windows performance engineers find
the project.

## License

MIT — see [LICENSE](LICENSE).
