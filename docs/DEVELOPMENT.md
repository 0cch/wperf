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
- `commands/`: CLI controllers and command-specific logic.

## Regression data

- `tests/data/cpu-stack.etl`: CPU samples + StackWalk.
- `tests/data/sched.etl`: CSwitch/ReadyThread/wakeup latency.
- `tests/data/syscall.etl`: syscall enter/exit.
- `tests/data/io.etl`: FileIo/DiskIo init/complete/orphan metrics.

Expected counts live beside each ETL. Replay tests do not require administrator
rights.

## Validation

```powershell
pwsh -NoProfile -File .\scripts\m1_smoke.ps1 -SkipRecord
pwsh -NoProfile -File .\scripts\m2_smoke.ps1 -SkipRecord
pwsh -NoProfile -File .\scripts\m4_smoke.ps1
pwsh -NoProfile -File .\scripts\m5_smoke.ps1
pwsh -NoProfile -File .\scripts\m6_smoke.ps1
pwsh -NoProfile -File .\scripts\m7_smoke.ps1
```

Hardware-dependent tests must be run on the target CPU/OS combination.
