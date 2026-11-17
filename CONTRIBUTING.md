# Contributing to wperf

Thank you for helping improve wperf. The project is a Windows-native ETW profiler
with a small, explicit surface area and a strong preference for evidence over
guesswork.

## Ways to contribute

| Area | Examples |
|---|---|
| Compatibility | Windows 10/11, Intel/AMD, VBS/HVCI, WOW64, multi-group, ARM64 |
| ETW and decoding | Kernel/classic events, cached stacks, private providers, diagnostics |
| Analysis | CPU, off-CPU, scheduler, I/O, PMC, LBR, heap, pool, DPC/ISR, driver spans |
| Symbols | PDB lookup, identity-aware caches, archive replay, partial downloads |
| Machine contract | CLI catalog, JSON schemas, CSV, exit codes, Agent errors, capabilities |
| Documentation | Usage, troubleshooting, architecture, compatibility, hardware evidence |
| CI and release | Build portability, deterministic tests, packaging, release automation |

Issues and pull requests are welcome. For substantial behavior changes, open an
issue first and describe the profiling workflow, expected ETW evidence, output
contract, and compatibility impact.

## Before you start

- Read [CODING_STANDARDS.md](docs/CODING_STANDARDS.md).
- Read [DEVELOPMENT.md](docs/DEVELOPMENT.md) for repository structure and test
  selection.
- Read [ARCHITECTURE.md](docs/ARCHITECTURE.md) before changing capture, replay,
  IPC, symbolization, or privilege handling.
- Check [TODO.md](docs/TODO.md) for current priorities.
- Search existing issues before opening a new one.
- Do not put raw ETLs, crash dumps, credentials, user names, host names, private
  paths, or other sensitive machine data in a public issue or pull request.
  Follow [SECURITY.md](SECURITY.md) for vulnerabilities.

## Development setup

The supported local toolchain is Visual Studio 2022, CMake 3.24+, Windows SDK,
PowerShell 7, and a vcpkg checkout.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static-md `
  -DVCPKG_MANIFEST_MODE=ON `
  -DWPERF_WARNINGS_AS_ERRORS=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

No-disassembly build:

```powershell
cmake -S . -B build-off -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static-md `
  -DVCPKG_MANIFEST_MODE=ON `
  -DWPERF_ENABLE_DISASM=OFF `
  -DWPERF_WARNINGS_AS_ERRORS=ON
cmake --build build-off --config Release --parallel
ctest --test-dir build-off -C Release --output-on-failure
```

## Tests and evidence

Normal CTest covers decoders, regression replay, CLI parsing, generated command
help, JSON contracts, Agent discovery/errors, diagnostics, and non-privileged
process behavior. Fixture-dependent ETW tests are generated at runtime; do not
commit captured traces solely to make a test pass.

For capture, live IPC, or hardware changes, configure and run the integration
tests from an elevated terminal:

```powershell
$env:WPERF_REQUIRE_LIVE_TESTS = "1"
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static-md `
  -DVCPKG_MANIFEST_MODE=ON `
  -DWPERF_RUNTIME_ETW_TESTS=ON `
  -DWPERF_WARNINGS_AS_ERRORS=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release -L integration --output-on-failure
```

Hardware-dependent changes need more than a synthetic test:

- record the Windows build, CPU model, topology, privilege state, VBS/HVCI, and
  relevant firmware or virtualization settings;
- state the exact capture and report commands and the workload;
- retain counts, periods, depths, loss counters, quality fields, and known
  limitations;
- report PASS, FAIL, UNSUPPORTED, and NOT-RUN separately;
- never treat a skipped or unsupported hardware test as a pass.

Use [HARDWARE_VALIDATION.md](docs/HARDWARE_VALIDATION.md) as the evidence
template. Review any attached evidence for sensitive machine and process data.

## Changing public surfaces

### CLI and generated help

The public command catalog lives in `src/cli.cpp`; typed requests live in
`src/requests.h` and `src/requests.cpp`. Add or change the option once, then add
parser and request-mapping tests.

Regenerate the checked-in command reference after changing a command or option:

```powershell
cmake --build build --config Release --target generate-command-reference
```

`tests/cli_conformance_test.ps1` fails when generated help or
`docs/COMMAND_REFERENCE.md` is stale.

### JSON, CSV, and Agent contracts

JSON/CSV fields, exit codes, quality semantics, generated capability metadata,
and structured Agent errors are public compatibility surfaces. See
[PUBLIC_CONTRACT.md](docs/PUBLIC_CONTRACT.md).

When changing them:

- update the relevant schema under `docs/schemas/`;
- update representative fixtures and `tests/json_contract_test.ps1`;
- update `tests/agent_friendly_test.ps1` for `capabilities --json`,
  `--json-errors`, or `record --json` changes;
- document additive fields, semantic changes, or deprecations;
- update [AGENT.md](docs/AGENT.md) when automation guidance changes;
- avoid parsing or promising stability for human-readable error wording.

`capabilities --json` is generated from the command catalog. Add semantic flags
such as elevation or global-state effects to the catalog rather than maintaining
a second hand-written inventory.

### Diagnostics and hardware features

Diagnostic captures and reports have additional ownership and boundary rules in
[DIAGNOSTICS.md](docs/DIAGNOSTICS.md). Changes to DPC/ISR, heap, pool, or driver
work should include payload/lifetime regressions and explicit semantics for
incomplete, estimated, or system-owned data.

## What makes a good change

- Preserve the controller/elevated-worker security boundary.
- Keep raw ETW values and derived estimates distinguishable.
- Preserve process/thread/image lifetimes and unknown attribution.
- Report unsupported capabilities as `UNSUPPORTED`, not as a successful pass.
- Add regression coverage for every bug fix where practical.
- Keep JSON/CSV fields, exit codes, and quality counters compatible or document
  the breaking change.
- Update the relevant durable docs: README, USAGE, DEVELOPMENT, ARCHITECTURE,
  COMPATIBILITY, PUBLIC_CONTRACT, AGENT, DIAGNOSTICS, RELEASE, or TODO.
- Do not add a custom driver or direct MSR access unless the project scope is
  deliberately changed. wperf is an ETW consumer, not a kernel profiler.

## Good first contributions

Good entry points include documentation fixes, sanitized replay fixtures,
decoder boundary tests, clearer structured errors, CLI conformance coverage,
schema tests, compatibility reports, and hardware validation on additional CPUs
or Windows builds.

The highest-value open areas are listed in [TODO.md](docs/TODO.md), including
platform coverage, cached/scheduler stacks, profiler coexistence, JIT method
lifetimes, provider-schema rendering, native FlameGraph output, and soak or
perturbation measurements.

## Pull request checklist

- [ ] The change is scoped and explained.
- [ ] New behavior has tests or documented hardware evidence.
- [ ] Normal CTest passes; elevated/live tests pass when relevant.
- [ ] New decoders or hardware paths have synthetic and live coverage as applicable.
- [ ] CLI catalog, generated command help, and command reference are synchronized.
- [ ] JSON/CSV schemas, exit codes, and Agent compatibility were considered.
- [ ] Public-contract or deprecation implications are documented.
- [ ] User, developer, Agent, diagnostic, and hardware documentation is current.
- [ ] Hardware limitations are explicit; skipped tests are not reported as passes.
- [ ] No sensitive traces, dumps, credentials, or machine-specific paths are
      committed.

## Maintainer review

Maintainers review scope, compatibility, privilege boundaries, data-quality
semantics, and evidence together. A change that alters JSON/CSV fields, exit
codes, Agent capabilities/errors, hardware claims, or the elevated-worker
boundary requires explicit compatibility review.

Security-sensitive reports should use the private advisory flow in
[SECURITY.md](SECURITY.md), not a public issue.

## License

Contributions are accepted under the MIT License in [LICENSE](LICENSE). No
separate CLA is required.