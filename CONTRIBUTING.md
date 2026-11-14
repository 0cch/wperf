# Contributing to wperf

Thank you for helping improve wperf. The project is a Windows-native ETW profiler
with a small, explicit surface area and a strong preference for evidence over
guesswork.

## Before you start

- Search existing issues and discussions before opening a new issue.
- For a substantial feature or behavior change, open an issue describing the
  workflow, expected ETW evidence, and compatibility impact before coding.
- Do not include raw ETLs, PDB paths, command lines, user names, or credentials
  in a public issue. Follow [SECURITY.md](SECURITY.md).

## Development setup

Follow [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md). The short version:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

The two live ETW tests require elevation. Run them from an elevated terminal
with `WPERF_REQUIRE_LIVE_TESTS=1`.

## What makes a good change

- Preserve the controller/elevated-worker security boundary.
- Keep raw ETW values and derived estimates distinguishable.
- Report unsupported hardware as `UNSUPPORTED`, not as a failed capture.
- Add regression coverage for every bug fix where practical.
- Keep output stable for scripts: JSON/CSV fields, exit codes, and quality
  counters are part of the public contract.
- Update the relevant durable documentation: README, USAGE, DEVELOPMENT,
  ARCHITECTURE, COMPATIBILITY, RELEASE, or TODO as appropriate.
- Avoid adding a custom driver or direct MSR access unless the project scope is
  deliberately changed. wperf is an ETW consumer, not a kernel profiler.

## Tests and evidence

At minimum, a code change should pass:

```powershell
ctest --test-dir build -C Release --output-on-failure
```

Hardware-dependent changes need more than a synthetic test:

- record the Windows build, CPU model, privilege state, and relevant firmware or
  virtualization settings;
- state the exact capture command and workload;
- retain counts, loss counters, decoded fields, and known limitations;
- do not treat a skipped or unsupported hardware test as a pass.

Use [docs/HARDWARE_VALIDATION.md](docs/HARDWARE_VALIDATION.md) as the evidence
template. Raw evidence should be attached only when the contents have been
reviewed for sensitive machine and process data.

## Good first contributions

Good entry points include documentation fixes, sanitized replay fixtures,
decoder boundary tests, clearer error messages, compatibility reports, and
hardware validation on additional CPUs or Windows builds.

Use the issue templates for bugs, feature requests, and hardware validation.
Normal pull requests receive the repository's CI checks automatically.
Security-sensitive reports should use the private advisory flow in
[SECURITY.md](SECURITY.md), not a public issue.

## Maintainer review

Maintainers review compatibility, privilege boundaries, quality semantics, and
evidence together. A change that alters JSON/CSV fields, exit codes, hardware
claims, or the elevated-worker boundary requires explicit compatibility review.
## Pull request checklist

- [ ] The change is scoped and explained.
- [ ] New behavior has tests or documented hardware evidence.
- [ ] `ctest` passes; elevated live tests have been run when relevant.
- [ ] JSON/CSV/exit-code compatibility was considered.
- [ ] User and developer documentation is updated.
- [ ] Hardware limitations are explicit.
- [ ] No sensitive traces, dumps, credentials, or machine-specific paths are
      committed.

## License

Contributions are accepted under the MIT License in [LICENSE](LICENSE). No separate CLA is required.
