# Coding Standards

These rules apply to all new code and to changes that touch existing code. They
exist to keep the ETW replay/capture boundary explicit, keep command behavior
reproducible, and keep machine-readable output maintainable.

## Required workflow

1. Start with a failing test for every behavior change.
2. Make the smallest implementation change that turns the test green.
3. Refactor only after the test is green.
4. Run the focused tests, then the non-live suite. Run elevated live tests when
   capture, session, or IPC behavior changed.
5. Update the durable contract documentation and schemas when output changes.

Do not use a successful build as a substitute for a test. Hardware-dependent
changes must also follow [HARDWARE_VALIDATION.md](HARDWARE_VALIDATION.md).

## Dependencies

- All third-party C++ dependencies must be declared in `vcpkg.json`.
- Use the repository's vcpkg checkout/toolchain in manifest mode:
  `x64-windows-static-md` for release and CI builds.
- Do not add `FetchContent`, external project downloads, or vendored source
  trees for a dependency that vcpkg can provide.
- Consume CMake packages through imported targets, for example
  `nlohmann_json::nlohmann_json` and `Zydis::Zydis`.
- Keep the baseline and dependency versions pinned in `vcpkg.json`.
- Update `THIRD_PARTY_NOTICES.md` when a dependency is added or its version or
  license changes.

Local configure command:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=D:/Codes/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static-md `
  -DVCPKG_MANIFEST_MODE=ON `
  -DWPERF_WARNINGS_AS_ERRORS=ON
```

## Command-line parsing

- CLI11 is the only parser for public commands.
- Command definitions, aliases, defaults, arity, and option descriptions belong
  in the CLI11 command catalog in `src/cli.cpp`.
- After CLI11 validation, construct one typed `CommandRequest` variant and pass
  it to the command implementation.
- Public command implementations must not scan the raw argument vector. Do not
  add new `OptionValue`, `HasFlag`, or ad-hoc `--option` loops outside the
  internal worker/guard entry points.
- Internal worker, guard, and live-worker command lines must also be parsed once
  into a typed internal request struct.
- Keep `--` target forwarding explicit and preserve target arguments byte for
  byte at the typed request boundary.
- Add parser tests for attached values (`-F99`), `--name=value`, aliases,
  Unicode paths, defaults, invalid numbers, and argument separators.

## JSON and schema output

- Use nlohmann/json as the JSON implementation. Do not hand-build JSON strings,
  concatenate JSON fragments, or maintain custom JSON escape/write routines.
- Use `nlohmann::ordered_json` for objects whose field order is contractual.
- Use `JsonStreamWriter` for large or incremental output. Do not materialize an
  entire report, trace, archive manifest, or event stream in a DOM.
- Use the shared `JsonStreamWriter` when emitting a metadata envelope with a
  quality object and a rows array. Do not write envelope punctuation manually.
- Keep 64-bit counters as integer values. Never route an integer through
  `double` merely to serialize it.
- Add or update a schema under `docs/schemas/` for every public JSON shape
  change. Add a fixture/contract test that validates the shape.
- Keep text and CSV output separate from JSON helpers. CSV quoting is not JSON
  escaping.
- Treat `null`, missing values, and zero-valued counters as distinct states and
  document the distinction in the schema.

## C++ and Windows code

- Target C++20 and keep `/W4 /permissive- /utf-8 /EHsc` clean. CI treats
  warnings as errors.
- Prefer standard library facilities, explicit ownership, RAII, and narrow
  interfaces. Do not introduce global mutable state without a documented
  lifetime or synchronization reason.
- Treat Windows handles and ETW resources with explicit cleanup paths. Preserve
  the controller/elevated-worker security boundary.
- Do not parse arbitrary files in the elevated worker. Put replay and file
  parsing in the normal-integrity controller.
- Keep 64-bit arithmetic explicit and check overflow before narrowing.

## Tests and contracts

- Every command option must have a typed-request assertion or an end-to-end
  test.
- Every decoder change needs malformed/truncated-input coverage.
- Every JSON schema change needs a checked-in fixture or an equivalent
  deterministic contract test.
- Every quality, loss, truncation, or exit-code change needs a process-level
  test.
- Do not edit generated `docs/COMMAND_REFERENCE.md` directly; regenerate it from
  the CLI catalog.
- Do not commit raw traces, dumps, credentials, user paths, or other sensitive
  diagnostic data without a documented redaction review.

## Documentation and release hygiene

- Update `USAGE.md`, `PUBLIC_CONTRACT.md`, `COMPATIBILITY.md`, or
  `ARCHITECTURE.md` when the user-visible behavior or system boundary changes.
- Update `CHANGELOG.md` for user-visible changes.
- Run `wperf doctor`, `wperf --version`, and the full non-live CTest suite before
  requesting review.
- Run elevated live tests when the change affects ETW capture, session cleanup,
  worker IPC, or live rendering.
- Keep `vcpkg.json`, CI workflows, release scripts, and development docs in
  sync. A dependency change that works locally but not in CI is incomplete.