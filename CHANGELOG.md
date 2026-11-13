# wperf Changelog

## 0.8.0.0
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
