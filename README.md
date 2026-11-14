# wperf

`wperf` is a lightweight Windows profiler built on Event Tracing for Windows
(ETW). It aims to bring the everyday `perf` workflow to Windows while keeping
the platform boundary explicit: no custom driver, no kernel patching, and no
pretence that ETW exposes every Linux `perf_event_open` feature.

Use it to capture CPU samples, call stacks, scheduler events, PMC samples,
LBR branches, and AMD64 context registers, then inspect the result as tables,
trees, JSON/CSV, or FlameGraph folded stacks.

> wperf is pre-1.0. CLI and machine-readable schemas are usable, but may still
> change between minor releases. Hardware and ETW capability differences are
> reported explicitly rather than hidden.

## Highlights

- CPU sampling with ETW call stacks and PDB-backed symbolization.
- Scheduler, syscall, page-fault, file/disk I/O, and off-CPU analysis.
- ETW PMC sampling, CSwitch counter snapshots, Intel LBR, and AMD64
  context-register decoding.
- Perf-style reports: flat tables, call trees, JSON/CSV, and folded stacks.
- Live `top` and `trace` workflows with an ordinary-integrity controller.
- Instruction heat and optional Zydis disassembly.
- Weighted profile diff and reproducible module/PDB archives.
- Verified module identity, symbol caching, inline frames, and archive replay.

## Requirements

- Windows 11, or Windows 10 21H2+.
- x64 process; Windows system ETW capture requires administrator rights.
- Visual Studio 2022 with the Desktop C++ workload.
- Windows SDK 10.
- CMake 3.24+.
- PowerShell 7 recommended for development and release scripts.

No WPA/WPR runtime is required to run the built executable. Disassembly is
provided by statically linked Zydis.

## Build

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

Build without Zydis when third-party disassembly is not needed:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DWPERF_ENABLE_DISASM=OFF
```

The default build uses CMake `FetchContent` for pinned Zydis/Zycore revisions,
so the first configure requires access to GitHub.

## Quick start

```powershell
# Capture all CPU samples for five seconds.
.\build\Release\wperf.exe record --elevate --duration 5000 -o cpu.etl

# Inspect hot functions and call stacks.
.\build\Release\wperf.exe report -i cpu.etl `
  --symbol-path .\build\Release --call-graph

# Capture a child process.
.\build\Release\wperf.exe record --elevate --duration 10000 `
  -o app.etl -- .\path\to\app.exe

# Produce FlameGraph input.
.\build\Release\wperf.exe report -i app.etl `
  --symbol-path .\build\Release --folded --quiet -o stacks.folded
```

Live views:

```powershell
.\build\Release\wperf.exe top --elevate --interval 1000
.\build\Release\wperf.exe trace --live --elevate -e sched --duration 5000
```

Off-CPU analysis:

```powershell
.\build\Release\wperf.exe record --elevate --profile sched --duration 5000 `
  -o sched.etl
.\build\Release\wperf.exe report -i sched.etl --off-cpu `
  --symbol-path .\build\Release
```

Hardware profiler examples:

```powershell
# ETW PMC sampling.
wperf record --elevate --pmc-source TotalCycles --period 1000000 `
  --duration 3000 -o pmc.etl

# Intel LBR on timer or PMC interrupts.
wperf record --elevate --lbr -F 99 --duration 3000 -o lbr.etl

# AMD64 context registers from an offline ETL.
wperf trace -i captured.etl -e context --json
```

## Privilege model

`record` and live ETW capture require elevation. `--elevate` launches one hidden
elevated worker; the interactive controller stays at its current integrity
level and owns parsing, symbolization, reporting, and terminal output. See
[SECURITY.md](SECURITY.md) and [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Compatibility and limits

- PMU results are ETW sample-derived unless explicitly documented otherwise.
- CSwitch counter snapshots are estimated deltas and are always marked
  `exact:false`.
- LBR depth is finite and CPU-dependent; it is not Intel PT.
- Context-register decoding is validated for AMD64; ARM64 decoding is not
  implemented.
- Syscall argument and kernel-internal memory semantics are not synthesized.
- Windows ETW cannot reproduce every Linux `perf_event_open` group or
  multiplexing behavior.

See [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md) for the full matrix.

## Documentation

- [Usage and command reference](docs/USAGE.md)
- [Development guide](docs/DEVELOPMENT.md)
- [Architecture](docs/ARCHITECTURE.md)
- [Compatibility](docs/COMPATIBILITY.md)
- [Public contract and versioning](docs/PUBLIC_CONTRACT.md)
- [JSON schemas](docs/schemas)
- [Hardware validation evidence](docs/HARDWARE_VALIDATION.md)
- [Roadmap and open work](docs/TODO.md)
- [Release process](docs/RELEASE.md)
- [Contributing](CONTRIBUTING.md)
- [Security policy](SECURITY.md)

Run `wperf help`, `wperf help <command>`, or `<command> --help` for the complete
option set for the installed build.

## Contributing

Bug reports, reproducible ETL fixtures, hardware validation, documentation
fixes, and code contributions are welcome. Start with
[CONTRIBUTING.md](CONTRIBUTING.md) and the open items in
[docs/TODO.md](docs/TODO.md). Hardware-dependent claims need reproducible
evidence; skipped or unsupported runs are not treated as passes.

## License

wperf is released under the MIT License. See [LICENSE](LICENSE).