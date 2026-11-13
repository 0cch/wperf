# wperf

`wperf` is a lightweight Windows profiling and tracing CLI built on ETW. It is
designed for workflows where WPA/WPR or vendor profilers are too heavy, while
still keeping a clear boundary around capabilities Windows exposes without a
custom driver.

## Capabilities

- CPU sampling, stack correlation, PDB/SHA256-aware module identity.
- Profile reports: perf-style table, call tree, CSV/JSON and FlameGraph folded stacks.
- Real-time top and live sched/syscall/pagefault/I/O/LBR tracing.
- Scheduler, syscall, page fault, file/disk I/O correlation and quality metrics.
- ETW PMC sampling, LBR payload decoding, context-register capability probing.
- `annotate`, weighted/calltree `diff`, reproducible `archive`.
- TDH provider enumeration and private-provider ETL capture.
- Optional Zydis disassembly, enabled by default.

## Build

Requirements: Windows 10 21H2+/Windows 11 x64, CMake 3.24+, Visual Studio 2022,
Windows SDK.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

Disassembly defaults to ON and fetches pinned Zydis/Zycore revisions. To build
without third-party disassembly:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DWPERF_ENABLE_DISASM=OFF
```

Create a release directory:

```powershell
pwsh -NoProfile -File .\scripts\build_release.ps1
```

## Quick start

```powershell
wperf --version
wperf doctor
wperf record --elevate --duration 3000 -o trace.etl
wperf report -i trace.etl --symbol-path .\build\Release --call-graph
wperf report -i trace.etl --symbol-path .\build\Release --folded --quiet -o stacks.folded
wperf top --elevate --interval 1000
wperf trace --live --elevate -e sched --duration 5000
wperf annotate -i trace.etl --limit 20 --disasm
wperf diff -1 before.etl -2 after.etl --calltree --json
wperf archive -i trace.etl -o archive --max-size-mb 512
wperf list providers --max 20
```

System capture requires administrator rights. `--elevate` starts one hidden
elevated worker; the normal-permission controller owns the terminal and sends
START/STOP over an authenticated named pipe.

## Help and flame graphs

Use `wperf help <command>` or `<command> --help` for complete options and
examples. FlameGraph-compatible collapsed stacks are produced by
`wperf report --folded`; SVG/HTML rendering remains an external step in this
release.

```powershell
wperf report -i trace.etl --symbol-path .\build\Release `
  --folded --quiet -o stacks.folded
flamegraph.pl stacks.folded > flamegraph.svg
```

`report -o` writes UTF-8 without BOM and LF line endings directly, so the
folding step requires no wrapper script. SVG rendering still uses the external
Brendan Gregg `flamegraph.pl`.

## Documentation

- [Usage](docs/USAGE.md)
- [Architecture](docs/ARCHITECTURE.md)
- [Security](docs/SECURITY.md)
- [Compatibility](docs/COMPATIBILITY.md)
- [Development](docs/DEVELOPMENT.md)
- [Release](docs/RELEASE.md)
- [Disassembly](docs/DISASSEMBLY.md)
- [Correctness and performance changes](docs/HARDENING.md)
- [Assessment completion audit](docs/ASSESSMENT_STATUS.md)
- [Main-branch follow-ups](docs/MAIN_FOLLOWUPS.md)
- [Hardware validation evidence](docs/HARDWARE_VALIDATION.md)
- [Issue acceptance status](docs/ISSUE_STATUS.md)
- [Remaining work and acceptance plan](docs/TODO.md)

## Boundaries

- No custom driver, kernel modification or MSR access.
- PMU results are ETW sample-derived unless explicitly marked exact.
- LBR/context-register availability depends on OS and CPU support.
- Syscall arguments and kernel-internal memory semantics are not synthesized.
- Unsupported or approximate data is reported explicitly.
