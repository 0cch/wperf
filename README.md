# wperf

Lightweight Windows profiling and tracing CLI built on ETW.

Current milestones: **M0-M3 implemented** (CPU sampling, stackwalk, PDB report).

- M0 capability probe: [docs/M0.md](docs/M0.md), [docs/M0_RESULTS.md](docs/M0_RESULTS.md)
- M1 foundation: [docs/M1.md](docs/M1.md)
- M2/M3 implementation status: [docs/M2_M3.md](docs/M2_M3.md)
- M2-M7 execution roadmap: [docs/ROADMAP.md](docs/ROADMAP.md)

## Build

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

## Initial commands

```powershell
.\build\Release\wperf.exe --version
.\build\Release\wperf.exe list
.\build\Release\wperf.exe doctor
.\build\Release\wperf.exe test
.\build\Release\wperf.exe record --duration 3000 -o .\trace.etl
```

`record` requires an elevated worker for system ETW capture. From a normal terminal use:

```powershell
.\build\Release\wperf.exe record --elevate --duration 3000 -o .\trace.etl
```

The elevated worker is headless. The normal-permission controller owns the console and talks to the worker over a private named pipe.

## M0

```powershell
pwsh -File .\scripts\m0_probe.ps1
```

An elevated shell additionally generates an xperf CPU sample + stack baseline.

## M1 scope

- C++20/CMake project and CLI skeleton.
- `list`, `doctor`, `test`, `record`, `recover`.
- ETW session start/stop and profile-source discovery.
- Headless elevated worker and session guard.
- Correlation IDs, JSONL logs and atomic state files.
- Crash dumps and basic recovery.
- Unit tests plus Windows CI build.

See [docs/PLAN.md](docs/PLAN.md) for the full design.
