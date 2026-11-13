# Development

## Configure and build

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

Disassembly is enabled by default. A no-third-party build is supported:

```powershell
cmake -S . -B build-nodisasm -DWPERF_ENABLE_DISASM=OFF
cmake --build build-nodisasm --config Release --parallel
ctest --test-dir build-nodisasm -C Release --output-on-failure
```

## Code boundaries

- `etw.*`: system ETW session and capability queries.
- `etl_reader.*`: ETL replay and payload decoding.
- `trace_events.*`: event correlation/quality.
- `symbolizer.*`: module identity and DbgHelp.
- `live_top.*`: real-time consumer.
- `disassembler.*`: optional Zydis integration.
- `commands/`: CLI controllers, central help/validation and command-specific logic.

## Regression data

- `tests/data/cpu-stack.etl`: CPU samples + StackWalk.
- `tests/data/sched.etl`: CSwitch/ReadyThread/wakeup latency.
- `tests/data/syscall.etl`: syscall enter/exit.
- `tests/data/io.etl`: FileIo/DiskIo init/complete/orphan metrics.

Expected counts live beside each ETL. Replay tests do not require administrator
rights.

## Validation

```powershell
ctest --test-dir build -C Release --output-on-failure
```

The CTest suite contains focused CLI, replay, archive, regression and live
integration coverage. Live ETW tests return skip code 77 unless the process is
elevated; CI sets `WPERF_REQUIRE_LIVE_TESTS=1` to turn an unexpected skip into a
failure. The former M1-M7 smoke scripts duplicated these checks and are no
longer part of the repository.

FlameGraph integration requires an external `flamegraph.pl` and Perl:

```powershell
wperf report -i tests\data\cpu-stack.etl `
  --symbol-path .\build\Release `
  --folded --quiet -o artifacts\stacks.folded

perl artifacts\FlameGraph\flamegraph.pl `
  artifacts\stacks.folded > artifacts\flamegraph.svg
```

Hardware-dependent tests must be run on the target CPU/OS combination.
