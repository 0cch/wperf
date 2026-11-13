# wperf Changelog

## 0.7.0
- Added complete `wperf help <command>` and richer `--help` pages.
- Added perf-style aliases including `-g`, `-s`, `-F`, `--stdio` and `--collapsed`.
- Added strict validation for conflicting output formats and missing option values.
- Made `record -g` explicit and added `record --no-call-graph`.
- Added process-rooted, percent-filtered FlameGraph folded stacks.
- Fixed `script` text/CSV headers, CSV quoting and `--symbol-path` handling.
- Added concise quality diagnostics with `--quiet` and `--verbose`.
- Added a Windows-safe FlameGraph wrapper that writes UTF-8/LF folded stacks.
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
