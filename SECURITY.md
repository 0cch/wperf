# Security Policy

## Supported versions

Security fixes are applied to the current `main` branch and the latest published
release. Pre-1.0 releases do not have a separate long-term-support branch.

## Reporting a vulnerability

Please use GitHub's **Private vulnerability reporting / Security advisory**
feature for this repository. Do not open a public issue containing exploit
details, credentials, sensitive ETL data, crash dumps, or machine-specific
paths.

Include, where possible:

- the affected version and Windows build;
- the command line and a minimal reproducer;
- whether capture was running elevated;
- impact and required preconditions;
- relevant logs with paths, user names, and process data redacted.

## Privilege model

Windows system ETW capture requires administrator rights and
`SeSystemProfilePrivilege`. wperf separates the process into:

- a normal-integrity controller that parses data, resolves symbols, owns the
  terminal, and writes reports;
- a hidden elevated worker that starts/stops ETW and returns bounded raw trace
  data over an authenticated local named pipe;
- a session guard that cleans up an orphaned ETW session if the worker exits.

Do not move DbgHelp/PDB parsing, arbitrary file access, or unbounded work into
the elevated worker.

## Sensitive trace data

ETL files can contain process names, command lines, module paths, user names,
SIDs, PIDs/TIDs, network names, file paths, and kernel/user addresses.
Diagnostic bundles and raw ETLs must be reviewed and redacted before sharing.

`doctor --bundle` excludes crash dumps by default. `--include-dumps` is a manual
opt-in and should be used only for private analysis.

## Scope and non-goals

wperf does not install a driver, patch the kernel, or read PMU MSRs directly.
It relies on documented Windows ETW interfaces. Reports about unsupported
hardware or missing kernel data are capability/quality issues unless they cross
a privilege boundary, corrupt state, or expose unauthorized information.

## Coordinated disclosure

Please allow maintainers time to investigate and prepare a fix before public
disclosure. A private advisory is the preferred place to coordinate timelines
and affected-version details.