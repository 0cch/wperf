# wperf Development Agent Guidelines and Practices

This document is for development agents that modify this repository's source code, tests, and documentation. It defines development practices, implementation methods, and validation workflows.
It is the auto-discovered guide for development agents in this repository and must be read and followed before starting work.

| Document | Intended agent | Responsibilities |
|---|---|---|
| Root `AGENTS.md` (this document) | Agents developing and maintaining wperf | Architecture constraints, coding methods, regression testing, documentation synchronization, and release workflow |
| [docs/AGENT.md](docs/AGENT.md) | Agents invoking wperf to analyze other programs | CLI usage, capability discovery, capture and reporting, error handling, and result interpretation |

The two documents are maintained separately. Update the caller guide when invocation behavior changes; keep development practices and methods in this document.
The user's current instructions take precedence over this document. Treat the current checkout as the source of truth for versions, dependencies, test registration, and feature status; do not repeat conclusions from previous sessions.

## 1. Project Scope

- A Windows-native C++20 performance-analysis tool that provides a command-line experience close to Linux perf, with ETW as its primary data source.
- It does not install a custom driver, directly read or write MSRs, or use kernel hooks; it does not depend on WPA/WPR at runtime.
- Target platforms are Windows 10 21H2+ and Windows 11 x64. Actual capabilities depend on hardware, OS version, permissions, and providers; do not claim a complete reproduction of Linux perf.
- Current coverage includes CPU, off-CPU, scheduling, I/O, PMC/LBR, context data, heap/pool, DPC/ISR, observable driver I/O, symbols, archives, and interactive flame graphs, among other areas.
- The runtime capability catalog is exposed by `wperf capabilities --json`; supported scope and limitations are documented in [COMPATIBILITY](docs/COMPATIBILITY.md) and [DIAGNOSTICS](docs/DIAGNOSTICS.md).

## 2. Starting Work

1. Use `pwsh`; first inspect `git status --short`, `git log -5 --oneline`, and the diff related to the task. Preserve the user's uncommitted changes.
2. Read [CODING_STANDARDS](docs/CODING_STANDARDS.md), [DEVELOPMENT](docs/DEVELOPMENT.md), and the documentation for the relevant modules.
3. Verify issues from older reports against the current source before treating them as current defects; historical conclusions are not a current bug list.
4. Identify affected CLI behavior, data semantics, schemas, permission boundaries, and tests before making a small, focused change.
5. For behavior changes, add a regression test that reproduces the issue before implementing the fix; for documentation-only changes, verify the content, links, and diff.
6. Run focused tests first, followed by non-live tests. Add administrator tests when capture, session cleanup, IPC, or live behavior is involved.
7. When delivering work, describe the changes, validation results, and unverified scope. Perform Git operations only when the task explicitly includes committing or releasing.

Prefer `rg` and `rg --files` for searches, and do not treat build directories or generated traces as source code.
If network access fails or is slow, try the SOCKS5 proxy at `127.0.0.1:7897`; do not modify the user's persistent proxy settings.
Do not commit ETLs, dumps, credentials, user paths, or diagnostic data that has not been reviewed for sensitive information.

## 3. Architecture and Code Entry Points

See [ARCHITECTURE](docs/ARCHITECTURE.md) for the complete design.

| Responsibility | Primary entry points |
|---|---|
| Program entry, CLI catalog, typed requests | `src/main.cpp`, `src/cli.*`, `src/requests.*` |
| Command dispatch and implementation | `src/commands.cpp`, `src/commands/` |
| ETW, elevation, IPC, state, and recovery | `src/etw.cpp`, `src/elevation.*`, `src/ipc.*`, `src/state.*`, `src/session_guard.*` |
| ETL replay, decoding, and shared models | `src/etl_reader.cpp`, `src/etw_decode.cpp`, `src/trace_model.*`, `src/trace_events.*` |
| CPU, off-CPU, and common analysis | `src/cpu_report.*`, `src/off_cpu_report.*`, `src/analysis.cpp` |
| Memory, DPC/ISR, and driver diagnostics | `src/diagnostic_*`, `src/commands/diagnostics.cpp` |
| Symbols, DbgHelp, and archives | `src/symbolizer.*`, `src/dbghelp_loader.cpp`, `src/archive.cpp` |
| Live data and UI | `src/live_top.cpp`, `src/live_data.cpp`, `src/commands/live_worker.cpp`, `src/top_renderer.cpp` |
| Flame-graph aggregation and HTML | `src/flamegraph.*`, `src/flamegraph_html.*`, `src/flamegraph_assets.h` |
| Compiler options and dependencies | `cmake/ProjectOptions.cmake` |
| Production targets, workloads, and documentation-generation targets | `cmake/WperfTargets.cmake` |
| Test registration | `cmake/TestHelpers.cmake`, `cmake/tests/{CoreTests,CliTests,FixtureTests}.cmake` |

### Permission and Lifetime Boundaries

- The normal-privilege controller owns the CLI, arbitrary file/PE/PDB parsing, ETL replay, symbolization, aggregation, rendering, and output.
- The hidden elevated worker is limited to ETW configuration, start/stop operations, and bounded raw capture/IPC. Do not move arbitrary file parsing into the worker.
- The guard owns cleanup of orphaned sessions created by this project and restoration of global profiling configuration. It must not stop unrelated sessions or blindly overwrite global state.
- The target process must not be launched until READY is complete. STOP must remain reachable during slow symbolization or rendering, with bounded waits and cleanup paths.
- `.restore` is an atomic worker recovery checkpoint independent of controller state. Recovery must validate the correlation ID and worker PID.
- Windows handles, sessions, and global state must be maintained through explicit ownership, RAII, and failure-path cleanup. Changes to these paths must cover cancellation, abnormal termination, and timeouts.

## 4. Data Trustworthiness: Invariants That Must Not Be Broken

New reports must reuse the same `TraceData` and replay pipeline rather than introduce a separate ETL parser.
Unknown, missing, ambiguous, unsupported, lost, and truncated data must remain visible in output and quality information. Never silently replace it with zero or an exact value.

### Identity, Mapping, and Sampling

- PIDs, TIDs, and addresses are reused; correlations must account for lifetimes and cannot rely only on numeric IDs or the latest mapping.
- In modern image payloads, PID 0 is a valid global owner. The capturing process PID in the header cannot replace the owner PID.
- Kernel mappings (PID 0/4) may be shared, while user-module mappings remain isolated by process.
- The start rundown is a baseline at the capture start, not the time when metadata arrived. The end rundown means the object still existed; it does not imply exit or unload.
- CPU sampling-period weights are in nanoseconds, while PMC weights are source-specific counts. Do not mix units.
- When the period is unknown, explicitly use a whole-report count fallback. Mixed sources require source selection and a clear weight explanation.
- Keep blocked and ready states distinct for off-CPU analysis. Mark inferred and capture-boundary intervals explicitly and clip them using actual exit times.
- LBR is not Intel PT. CSwitch PMC deltas are estimates; preserve quality semantics such as `exact:false`.

### Diagnostic Capabilities

- DPC/ISR and pool events are system-level activity; execution context must not be automatically converted into process ownership.
- Heap analysis targets an existing PID and primarily observes native allocations after attach. Module snapshots taken during attach must be labeled separately and obtained by the normal-privilege controller.
- Pool capture requires a four-character tag. Use `list --pool-tags` to discover tags, and impose resource limits on high-event-rate paths.
- Observing unreleased allocations does not prove a leak. Explain the capture window, lost events, and boundary state.
- Driver I/O correlates observable call spans through IRPs, unique match IDs, and operation families. Do not claim to isolate device service time or per-layer minifilter latency.
- Mark the provenance of process, thread, and stack context. Do not present inferred provenance as exact attribution.

### Large Inputs, Output, and Flame Graphs

- Keep processing streaming and use explicit resource budgets. Avoid building complete JSON DOMs or huge intermediate strings for large traces.
- Do not serialize 64-bit counters through `double`; HTML/JavaScript paths must also protect values above `2^53`.
- Flame graphs use flat node storage; deep stacks and untrusted input must not cause recursive destruction or traversal stack overflow.
- Preserve unresolved-address counts, original ancestor chains after zoom, copied stacks, and reimport semantics. New interactions must retain these regression scenarios.
- Text output follows the existing UTF-8-without-BOM and LF contract. Escape HTML, embedded scripts, and user content according to their respective contexts.

## 5. CLI, Public Contracts, and Documentation Synchronization

[PUBLIC_CONTRACT](docs/PUBLIC_CONTRACT.md) is the source of truth for public contracts.

- CLI11 is the only public argument parser. Commands, aliases, defaults, arity, help, and capability definitions belong in `src/cli.cpp`.
- After validation, map arguments into typed requests in `src/requests.*`. Command implementations must not rescan raw public arguments.
- Internal worker, guard, and live-worker arguments must also be parsed once and converted into typed structures.
- Preserve target arguments after `--`. New argument tests must cover attached values, `--name=value`, aliases, Unicode paths, invalid numbers, and separators.
- Use nlohmann/json; use the shared `JsonStreamWriter` for large output. Do not hand-build JSON or duplicate escaping and envelope logic.
- Missing, `null`, and zero have different meanings in JSON. Maintain `docs/schemas/` and deterministic contract tests together.
- Automation must check exit codes and report quality. A successful `record --json` only proves that the capture operation completed; it does not by itself prove that analysis is trustworthy.
- Keep the stable machine interfaces of `capabilities --json`, global `--json-errors`, and `record --json`.

| Exit code | Meaning |
|---:|---|
| 0 | Request completed |
| 2 | Invalid or conflicting arguments |
| 3 | Insufficient permission or elevation cancelled |
| 4 | Capture, worker, session, guard, or cleanup failure |
| 5 | Requested capability or source has no available data |
| 6 | Replay failed or a quality gate was not met |
| 7 | Target launch or access failure |
| 8 | Target exited with a nonzero status; the original status is reported separately |
| 9 | Internal, IPC, filesystem, or unexpected exception |

When changing command names, arguments, default behavior, enums, column names, exit codes, or quality fields, also check:

- [README](README.md), [USAGE](docs/USAGE.md), and [PUBLIC_CONTRACT](docs/PUBLIC_CONTRACT.md).
- [Agent usage](docs/AGENT.md), [DIAGNOSTICS](docs/DIAGNOSTICS.md), and [COMPATIBILITY](docs/COMPATIBILITY.md).
- Architecture, development, hardware-validation, and release documentation; `docs/schemas/`; relevant tests; and [CHANGELOG](CHANGELOG.md).

`docs/COMMAND_REFERENCE.md` is generated from the catalog and must not be edited by hand. After a CLI change and build, run:

```powershell
cmake --build build --config Release --target generate-command-reference
```

Review the generated diff and run the CLI conformance, typed-request, and machine-contract tests.
Do not maintain a second static capability inventory in new documentation.

## 6. Build and Dependencies

Requirements: Windows x64, the VS2022 C++ toolchain and Windows SDK, CMake 3.24+, PowerShell 7, and vcpkg.
Node.js is used for the flame-graph browser contract. Windows Performance Toolkit can be used for independent comparison but is not a runtime dependency.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static-md `
  -DVCPKG_MANIFEST_MODE=ON `
  -DWPERF_WARNINGS_AS_ERRORS=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
.\build\Release\wperf.exe --version
.\build\Release\wperf.exe doctor
```

- Dependencies are fixed by `vcpkg.json`: CLI11, nlohmann-json, and optional Zydis. Use imported targets and do not add vendored or FetchContent dependencies that vcpkg can provide.
- Keep C++20 and `/W4 /permissive- /utf-8 /EHsc`; enable warnings-as-errors during validation.
- Read the baseline from the manifest. Do not change it casually to bypass local failures. On parse failures, inspect the vcpkg checkout and version database; fetching a commit does not necessarily update the database in an older checkout.
- When adding or changing dependencies, synchronize the manifest, [THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES.md), CI, and development documentation.
- Validate no-disassembly builds in a separate directory, such as `build-off`, adding `-DWPERF_ENABLE_DISASM=OFF`.
- The root `CMakeLists.txt` owns version and module assembly. Add new targets and tests to the appropriate module; do not move them back into the root file.

## 7. Testing and Performance Validation

- CTest does not generate runtime ETW fixtures by default; `WPERF_RUNTIME_ETW_TESTS` defaults to OFF.
- `WPERF_REQUIRE_LIVE_TESTS=1` affects that default at configure time, but an existing cache may still be OFF. Full administrator validation must enable it explicitly.
- In an administrator PowerShell session, reuse the complete configure parameters above and set the option shown below. The environment variable is process-local; restore its previous value afterward.

```powershell
$env:WPERF_REQUIRE_LIVE_TESTS = "1"
cmake -S . -B build -DWPERF_RUNTIME_ETW_TESTS=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

The second line reuses an already configured `build` directory and toolchain. For a first configuration, use the complete command from the previous section with `-DWPERF_RUNTIME_ETW_TESTS=ON`.
When a UAC action is required, explain what is being run, wait for the user to complete it, and then continue checking the results.

- `tests/generate_etw_fixtures.ps1` and controlled workloads generate fixtures under `<build>/generated-etw`; CTest owns setup and cleanup. Do not commit ETLs or `*.expected.json` files.
- With runtime tests disabled, replay-contract tests that depend on fixtures are disabled. Passing ordinary tests does not mean those paths were validated.
- `ctest -L integration` is useful for focused live checks but is not a substitute for the full CTest suite because not all replay-contract tests carry that label.
- Capture and live tests run serially. Avoid compilation or other high-load operations during performance measurements.
- Tests assert schemas, correlations, boundaries, quality, and units; do not hard-code machine-dependent event counts, hashes, or timings.
- Decoder changes must cover malformed and truncated input. Changes to quality, loss, truncation, or exit codes need process-level validation.
- Reports must distinguish PASS, FAIL, UNSUPPORTED, and NOT-RUN. Skips, disabled tests, unregistered tests, or empty captures are not passes.
- If Node is not on PATH, `cli_flamegraph_browser_contract` is not registered; verify that the test actually exists. Its DOM simulation does not replace real-browser layout, interaction, and export validation.
- Flame-graph regressions must emphasize deep input, resource limits, uint64 precision, ancestor chains after zoom, copy/reimport behavior, and HTML escaping.
- For timeouts, first inspect symbol-server networking, SDK symsrv, PDB matching, and cleanup paths; do not merely increase the timeout.
- If an offline test needs to clear `_NT_SYMBOL_PATH`, change only the process environment and document the scope; do not alter the user's persistent configuration.

Hardware-related changes follow [HARDWARE_VALIDATION](docs/HARDWARE_VALIDATION.md).
Performance conclusions must include the binary version/commit, build configuration, target, capture scope, symbol coverage, lost events, and quality gates.
When self-profiling, distinguish the controller, elevated worker, and guard; do not interpret an uncaptured process or data domain as evidence of zero overhead.
When reproducing a release, inspect the manifest and file hashes to avoid drawing conclusions from a stale build.

## 8. Commits and Releases

Perform commits, pushes, or releases only when the user requests them; documentation work alone does not trigger a release.
Use the `codex/` prefix by default for new branches, preserve the user's current branch and changes, and avoid unrelated refactors.
See [RELEASE](docs/RELEASE.md) for the full process, and do not reuse tag operations from previous sessions.

- The release order is fixed: update versions and documentation, run the full test suite, commit, push `main`, create the three-part tag, and wait for the release workflow.
- Do not create a release tag while changes are uncommitted or `main` has not been pushed successfully.
- Keep release notes as local release artifacts by default and update them with `gh release edit <release-tag> --notes-file <local-file>`; do not commit the release-notes file unless the user requests it.
- After publishing, verify the generated four-part tag, non-draft/non-prerelease status, manifest version/commit, and all asset hashes.
- Versions use `MAJOR.MINOR.PATCH.BUILD`; see PUBLIC_CONTRACT for compatibility policy. Breaking behavior must be documented explicitly.
- When updating the version, verify `CMakeLists.txt`, `vcpkg.json`, the defaults in `scripts/build_release.ps1`, CHANGELOG, and related documentation.
- Tags that trigger the Action must use the three-part form `vMAJOR.MINOR.PATCH`; the fourth component is generated from `GITHUB_RUN_NUMBER`, and the workflow creates the final four-part release tag.
- Do not manually push four-part input tags, and do not move or reuse published tags.
- When the user requests a release, keep following the Action and verify that the Release is non-draft and non-prerelease, and that the manifest version/commit and asset hashes are correct.
- Required assets are `wperf.exe`, `wperf.pdb`, `manifest.json`, and `SHA256SUMS.txt`.
- After the workflow succeeds, update release notes with `gh release edit <generated-release-tag> --notes-file <local-notes-file>`.

## 9. Maintaining This Document

Update this document when architecture boundaries, CLI flows, build modules, test switches, or release rules change.
Keep detailed specifications in the existing documents. This document should retain only onboarding entry points and easy-to-miss constraints; do not hard-code the current version number, total test count, or stale defect lists.