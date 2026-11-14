# wperf

**A lightweight Windows profiler built on ETW — closer to the `perf` workflow,
without a custom driver.**

[![CI](https://github.com/0cch/wperf/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/0cch/wperf/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/0cch/wperf?sort=semver)](https://github.com/0cch/wperf/releases)
[![License: MIT](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Windows%2010%2F11-blue.svg)](docs/COMPATIBILITY.md)

`wperf` captures CPU samples, call stacks, scheduler events, page faults, I/O,
off-CPU time, PMC samples, Intel LBR branches, and AMD64 context registers. It
then gives you perf-style tables, call trees, JSON/CSV, and FlameGraph folded
stacks — with explicit quality and compatibility metadata.

> Pre-1.0: useful today, but CLI and machine-readable fields can still evolve.
> Compatibility rules are documented in [PUBLIC_CONTRACT.md](docs/PUBLIC_CONTRACT.md).

## Why wperf?

WPA/WPR and PerfView are excellent full-featured tools. wperf focuses on the
developer loop:

- **Fast command line:** record, report, top, trace, diff, annotate.
- **Scriptable output:** deterministic exits, JSON schemas, CSV, folded stacks.
- **Clear semantics:** missing, approximate, lost, and estimated data stay visible.
- **Small trust boundary:** only a hidden worker runs elevated.
- **No kernel component:** ETW only; no driver, patching, or PMU MSR access.
- **Windows-native:** PDBs, ETW stacks, process/thread lifetimes, and ETW quality counters.

## What can it do?

| Workflow | Commands |
|---|---|
| Find CPU hotspots | `record`, `report`, `top` |
| Inspect call trees and FlameGraphs | `report --call-graph`, `report --folded` |
| Diagnose stalls and scheduling | `record --profile sched`, `report --off-cpu`, `trace -e sched` |
| Inspect syscalls, faults, and I/O | `trace -e syscall`, `trace -e pagefault`, `trace -e io` |
| Hardware profiling | PMC intervals, CSwitch snapshots, Intel LBR, AMD64 context registers |
| Compare before/after | `diff --calltree` |
| Reproduce symbols later | `archive`, `buildid-list`, `.symcache` |
| Inspect instructions | `annotate --disasm` with optional Zydis |

## 60-second tour

```powershell
# 1. Build
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel

# 2. Capture your app
.\build\Release\wperf.exe record --elevate --duration 5000 `
  -o app.etl -- .\path\to\app.exe

# 3. Show hot functions and call stacks
.\build\Release\wperf.exe report -i app.etl `
  --symbol-path .\build\Release --call-graph
```

Illustrative report shape:

```text
Overhead  Samples  Command     Module       Symbol
  42.1%     1840   app.exe     app.exe      render_frame
  17.4%      762   app.exe     app.exe      decode_packet
  11.8%      516   ntdll.dll   ntdll.dll    RtlAllocateHeap
```

Generate flame graph input:

```powershell
.\build\Release\wperf.exe report -i app.etl `
  --symbol-path .\build\Release --folded --quiet -o stacks.folded
perl flamegraph.pl stacks.folded > stacks.svg
```

## Common workflows

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
wperf report -i app.etl --json --metadata --strict-quality
wperf trace -i sched.etl -e sched --json --max 100
```

### Hardware counters and branch history

```powershell
wperf record --elevate --pmc-source TotalCycles --period 1000000 `
  --duration 3000 -o pmc.etl
wperf record --elevate --lbr -F 99 --duration 3000 -o lbr.etl
wperf trace -i captured-with-context.etl -e context --json
```

## Build requirements

- Windows 11 or Windows 10 21H2+ x64.
- Visual Studio 2022 with the Desktop C++ workload.
- Windows SDK 10 and CMake 3.24+.
- PowerShell 7 recommended for development and release scripts.
- Elevation for system capture; offline replay runs at normal integrity.

The release executable uses Windows system DLLs only. Zydis is statically linked.
The first CMake configure fetches pinned Zydis/Zycore revisions unless
`-DWPERF_ENABLE_DISASM=OFF` is used.

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
- [Development guide](docs/DEVELOPMENT.md)
- [Architecture and trust boundaries](docs/ARCHITECTURE.md)
- [Public contract and versioning](docs/PUBLIC_CONTRACT.md)
- [JSON schemas](docs/schemas)
- [Compatibility matrix](docs/COMPATIBILITY.md)
- [Roadmap and open work](docs/TODO.md)
- [Release process](docs/RELEASE.md)
- [Contributing](CONTRIBUTING.md)
- [Security policy](SECURITY.md)

## Contributing

Contributions are welcome across code, documentation, replay fixtures, decoder
tests, and hardware validation. Good entry points:

- add a sanitized ETL fixture or regression case;
- test a CPU/Windows/VBS combination;
- improve symbolization or output diagnostics;
- implement an item from [docs/TODO.md](docs/TODO.md);
- improve examples and troubleshooting.

Start with [CONTRIBUTING.md](CONTRIBUTING.md) and
[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md). If wperf saves you time, a star helps
other Windows performance engineers find the project.

## License

MIT — see [LICENSE](LICENSE).