# Security

- `record`, `top` and `trace --live` use ETW/Win32 system capabilities only.
- Target processes are not modified or instrumented.
- `CreateProcessW` is used directly; commands are never routed through a shell.
- The elevated worker is headless and communicates over a named pipe with:
  - a random correlation ID,
  - a one-time nonce,
  - an ACL restricted to owner/administrators/System,
  - controller PID validation using the named-pipe server PID.
- versioned structured live windows with bounded message and receive buffers.
- The controller can terminate only the worker process it created.
- ETL payload readers validate lengths before every field read.
- DbgHelp does not execute source-server commands; wperf only consumes local or
  symbol-server-resolved symbol data.
- Trace modules and PDBs are parsed as data, not loaded into the process.
- Live DbgHelp, PDB resolution, archive lookup, and formatting run in the
  ordinary-permission controller, not in the elevated worker.
- The elevated worker does not accept arbitrary file-read or download requests.
- Diagnostic bundles skip `.dmp` files unless explicitly requested with
  `--include-dumps`; dumps may contain process memory and must be handled as
  sensitive.
- JSON/log bundles may contain paths, command lines, process IDs and user names.
  Review them before sharing.
- Unknown/unsupported ETW capabilities are reported instead of emulated with
  unsafe kernel techniques.
