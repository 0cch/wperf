# wperf Changelog

## 0.12.0 - 2026-11-17

- Added `report --flamegraph` and standalone `wperf flamegraph` support for self-contained interactive HTML FlameGraphs from CPU, off-CPU, PMC, or existing folded-stack data.
- Added automatic weight labels and trace-quality detail to generated FlameGraph HTML; the format requires no Perl, external JavaScript, or network access.
- Preserved the standard `report --folded` output and documented `flamegraph.pl stacks.folded > stacks.svg` as a parallel SVG workflow.
- Fixed `--symbol-path` incorrectly replacing `_NT_SYMBOL_PATH`; explicit local paths now augment the environment symbol path, restoring Microsoft module PDB resolution.
- Added repeatable/semicolon-separated `--symbol-path` values for local PDB searches.
- Expanded interactive HTML FlameGraphs with frame details, newline-separated stack display/copy, horizontal stack scrolling, folded-stack copy, drag range zoom, search navigation, zoom navigation, minimum-percentage filtering, and SVG/PNG export of the current view.
- Split folded parsing, HTML presentation, and inline web assets into separate components and redesigned the HTML workspace with a persistent details sidebar, breadcrumbs, and current-view export.
- Added detailed symbol diagnostics to `report --verbose` and `report --json --metadata`, separating memory cache, persistent `.symcache`, and DbgHelp resolution work while exposing module rejection and PDB state.
- Added full DbgHelp, symbol-path, symcache, module, cache-byte, eviction, and unresolved-address diagnostics without redacting local user paths.
- Clarified that `doctor --bundle` produces a diagnostic bundle for review before sharing rather than performing automatic redaction.

## 0.11.0 - 2026-11-17

- Added `capabilities --json` as a schema-versioned Agent discovery surface generated from the command catalog.
- Added global `--json-errors` with stable error codes, retryability, remediation actions, and a JSON schema.
- Added `record --json` capture results with session/correlation identity, trace metadata, target status, and explicit unevaluated quality.
- Added Agent usage guidance and contract coverage for discovery, structured errors, bounded automation, and artifact-first workflows.

## 0.10.0.0 - 2026-11-16

- Added dedicated DPC, ISR, native heap, kernel pool, and driver ETW capture profiles and text/JSON/CSV diagnostic reports.
- Preserved system ownership for interrupts/pool, explicit allocation baselines and observed-unfreed semantics, and event-bounded driver call spans without inferring minifilter/device latency.
- Corrected delayed start-rundown metadata to an initial process/thread/image baseline, restoring early interrupt-module and scheduler-context attribution.
- Added selective diagnostic replay, bounded lifecycle/call correlation, schema/CLI/raw-payload regressions and elevated native workload smoke coverage.
- Added pool tag snapshot discovery and required tag filtering; avoided unrelated high-rate memory/page-list events and labeled scheduler-derived driver contexts.
- Avoided unnecessary per-driver-call stack walking and synchronized CLI help, generated reference, diagnostic contracts and compatibility documentation.

## 0.9.0.0 - 2026-11-16

- Preserved global image PID 0 in modern ETW image payloads instead of replacing it with the collector's header PID, restoring target kernel-module attribution in offline and live reports; added raw-payload/lifetime/report regressions.

- Separated worker recovery checkpoints from controller status and validated guard checkpoint identity.
- Corrected CPU table/tree/folded overhead to period weights; added `--weight period|samples` and `--source`, with whole-report fallback for unknown periods and rejection of mixed domains.
- Applied I/O/PMC PID/TID/comm/children/CPU/time selection, preserved PMC baselines across filters, added PMC CSV, and unified loss/strict gates; incompatible options now fail explicitly.
- Forwarded parsed `record --pmc-counters` sources to the worker; the typed-request conversion had silently dropped the counter list.
- Finalized stream quality after consuming samples and distinguished unloaded stacks from missing stacks.
- Waited for metadata-provider enablement before writing capture periods/counter sources, preventing successful writes that silently logged no metadata.
- Added default `diff --match symbol` cross-build alignment and optional strict identity matching, with explicit added/removed rows.
- Preserved recorded ETL module identity, separated local identity, and changed buildid status to matching/mismatched/missing.
- Cached module metadata and optional local-file SHA256; `buildid-list --hash` now requests hashing instead of hashing every mapping by default.
- Clipped inferred off-CPU intervals at thread/process exit and finalized derived quality counters.
- Implemented `script -o` UTF-8/LF output and observation-delta thread stat output in text/JSON/CSV, retaining observed exited threads; corrected PMC name examples to use `-e`.
- Updated help, generated command reference, usage, compatibility/public contracts, schemas, and regression tests together.

Compatibility: CPU/folded defaults use periods; use `--weight samples` for the prior count mode. Diff defaults to symbol matching; use `--match identity` for prior keys. Buildid PE fields now describe the capture, status values changed, and hashes require `--hash`; CSV adds local-identity/hash columns. These pre-1.0 default/meaning changes are released as a minor version rather than a compatible patch.

## 0.8.2.0 - 2026-11-16

- Normalized and documented the complete exit-code contract, including target launch, target failure, permission, and internal errors.
- Made trace truncation explicit, changed `--max` to unlimited by default, and added `trace --json --metadata`.
- Added a shared trace-quality finalizer for full, stream, metadata, and live paths; LBR decode failures and budget exhaustion now affect completeness.
- Reworked LBR replay to retain samples and ordered branch groups instead of flattening every branch into a full event.
- Added precise replay budget fields, skipped off-CPU derivation unless requested, and converted CPU row overflow into a replay-quality error.
- Added structured JSON/CSV output for `stat`, `sched`, `buildid-list`, `list`, and `doctor`, plus `stat --interval`.
- Added instruction-level annotate JSON/CSV fields, inline-frame reporting for off-CPU frames, weighted/per-trace-PID diff options, and unknown provider/opcode accounting.
- Expanded JSON schemas and contract tests for reports, traces, scheduler, stat, annotate, diff, archive, module identity, list, and doctor.
- Added a deterministic decoder fuzz smoke test and warnings-as-errors CI configuration.
- Replaced vendored/FetchContent dependencies with the pinned vcpkg manifest and `x64-windows-static-md` static dependency builds.
- Made CLI11 the single public parser and moved command dispatch to typed request variants; internal worker, guard, and live-worker arguments are also parsed once.
- Standardized JSON serialization on nlohmann/json and the shared streaming writer, including report metadata envelopes, PMC rows, and live trace events.
- Added repository coding standards, vcpkg-based development/release commands, and live PMC/live trace JSON integration tests.
- Made CI fetch the manifest baseline commit when GitHub's preinstalled vcpkg checkout does not contain it.
- Made the exit-code test assert based on the current process elevation state so elevated CI runners are tested correctly.
- Increased the target-exit test duration to avoid a startup race on busy CI runners.


## 0.8.1.0
- Optimized off-CPU report generation from 1.880 s to 0.657 s on the validation workload (64.8% less time, 2.84x throughput) by removing repeated linear image scans, interning module identities, using structured aggregation keys, and switching to reserved unordered maps.
- Reused indexed image/process lookups in annotate and off-CPU reporting, with deterministic tie-breaking preserved for equal sample counts.
- Added regression coverage for off-CPU module identity/RVA retention, identical-frame aggregation, and PID-reuse process instance starts.
- Fixed system-module symbol resolution by making device-path normalization case-insensitive and delay-loading a symsrv-capable DbgHelp from `WPERF_DBGHELP_PATH`, the Windows SDK Debugging Tools, or the application directory.
- Added `_NT_SYMBOL_PATH`/symbol-server validation on Intel hardware, including downloaded public PDBs and real `ntdll`/`kernel32` symbols.
- Added a CLI11-backed command catalog used for option validation, aliases, text help, generated Markdown reference, and consistent option-value normalization.
- Added exhaustive CLI unit tests for every public option plus checked-in command-reference conformance testing.
- Documented attached short values, `--name=value`, target forwarding with `--`, and the command-reference regeneration workflow.

## 0.8.0.0
- Hardware-validated timer/PMC LBR, ETW PMC source/period/snapshot semantics and AMD64 context-register decoding on Intel Core i5-14600K; added the legacy LBR API fallback required by that platform.
- Fixed elevated PMC source parsing, profile-source restore, ETL header lifetime, PMC snapshot source metadata, zero-valued legacy LBR timestamps, and added `trace -e context` AMD64 register output.
- Added MIT licensing, public compatibility policy, JSON schemas with CTest validation, and GitHub issue/PR templates for open-source contributions.
- Reorganized the durable documentation for open-source contributors, added contributing/security guides, and removed obsolete milestone and review-status documents.
- Preserved confirmed blocked time and reused-TID ready intervals; retained separate wakeup identities/stacks and original stack capture times in off-CPU reports.
- Made live renderer drain deadlines and exceptions fail with incomplete-output diagnostics; field truncation now marks live windows incomplete.
- Fixed CSwitch incoming/outgoing attribution, off-CPU instance/module-aware aggregation, truncated-boundary accounting, capture-relative interval cropping, and off-CPU quality gates found in review-4392026.
- Bounded live IPC metadata and stack payloads, kept STOP reachable while symbols render, and added full controller decode/render live smoke coverage.
- Moved live-top DbgHelp, PDB, archive, symbol cache and formatting into the ordinary-permission controller using bounded structured IPC.
- Added scheduler-event stack capture and offline off-CPU blocked/ready interval analysis with text, JSON, CSV and folded output.
- Added controller-side live archive resolution through `top --archive`.
- Added native `report -o FILE` output with UTF-8/LF folded stacks and removed the FlameGraph wrapper script.
- Added tag-driven GitHub release builds with four-part `vMAJOR.MINOR.PATCH.BUILD` versions.
- Removed duplicated M1-M7 smoke steps and scripts; focused CTest coverage is now authoritative.
- Added unknown-period and full-identity aggregation fixes, inline expansion, dedicated symcache and epoch-aware stack references.
- Added verified archive loading, sample streaming, PMC event snapshots, report selectors, scheduler/I/O quantiles and function heat.
- Moved lifecycle work out of the ETW callback lock, added managed budgets, early recovery checkpoints and owned-worker teardown deadlines.
- Added elevated live/fault integration that CI requires instead of accepting access-denied smoke results.
- Fixed live syscall/thread attribution, cross-window correlation and controller/worker cleanup.
- Isolated DbgHelp sessions, indexed module/name lookup, reused module identities and avoided unused stack aggregation.
- Enabled source-aware PMC interrupt sampling and LBR triggers; added native PMC periods and zero-event validation.
- Waited for capture READY before launching targets; added conditional sampling rollback and live session guards.
- Preserved sampling period/stack provenance; added exact-stack reports, JSON quality metadata and strict-quality mode.
- Validated annotate/archive identities, found matching PDBs and prevented archive filename collisions.
- Added raw-event/lifecycle regressions and JSON replay contracts; improved hardware smoke and soak accounting.

## 0.7.1
- Display ETW `UINT32_MAX` process IDs as `[unattributed]` in text, JSON, CSV and folded stacks.
- Added `top --symbol-path` and propagated it to the elevated live worker.
- Added `unattributed` and `unknown_events` to the `top` quality summary.
- Aligned `report`, `top`, `diff` and `annotate` text tables with bounded columns.
- Added regression coverage for unattributed samples and CLI output.

## 0.7.0
- Added complete `wperf help <command>` and richer `--help` pages.
- Added perf-style aliases including `-g`, `-s`, `-F`, `--stdio` and `--collapsed`.
- Added strict validation for conflicting output formats and missing option values.
- Made `record -g` explicit and added `record --no-call-graph`.
- Added process-rooted, percent-filtered FlameGraph folded stacks.
- Fixed `script` text/CSV headers, CSV quoting and `--symbol-path` handling.
- Added concise quality diagnostics with `--quiet` and `--verbose`.
- Added a Windows-safe UTF-8/LF folded-stack workflow.
- Added CLI and folded-stack regression tests.

## 0.6.0
- Added optional Zydis v4.1.1 x86/x64 disassembly, enabled by default.
- Added `annotate --disasm` with text, JSON and CSV output, plus an address-heat fallback.
- Added controller PID validation to elevated record/live workers.
- Restricted diagnostic bundles from including crash dumps by default and documented redaction limits.
- Replaced quadratic ETL process/thread/image lookups with incremental indexes.
- Added release documentation, third-party notices and a no-disassembly CI build.

## 0.5.0
- Added weighted and call-tree diffs with PID/DSO filters and CSV/JSON output.
- Added source-aware annotation and richer archive manifests.
- Added provider enumeration and private-provider capture.
- Added reproducible release packaging with compatibility metadata.

## 0.4.0

- Added ETW PMC profile source selection and experimental `stat --pmu`.
- Added LBR configuration probe via `TraceConfigureLastBranchRecord`.
- Added `annotate`, `diff`, `archive`, and release build workflow.
- Kept unsupported hardware capability results explicit.

## 0.3.0

- Added `stat`, snapshot `top`, and `script`.
- Added scheduler, syscall, page-fault, and I/O event model.
- Added offline `trace` and `sched` reports.

## 0.2.0

- Added ETL replay, classic ETW decoding, Sample/StackWalk pairing.
- Added DbgHelp symbolization, folded/calltree reports, filters, and buildid-list.

## 0.1.0

- Added ETW session lifecycle, controller/worker/guard, state, logs, crash dumps.
