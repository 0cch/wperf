# Compatibility

| Area | Support |
|---|---|
| OS | Windows 10 21H2+ and Windows 11 x64 |
| Toolchain | Visual Studio 2022, CMake 3.24+, Windows SDK 10 |
| CPU sampling | ETW system logger |
| Stack correlation | Direct StackWalk + modern StackWalkRef/Def |
| Symbols | DbgHelp, local PDB, optional symbol server |
| Scheduler/syscall/I/O | ETW classic events |
| PMU | ETW profile sources; sample-derived semantics |
| LBR | Available only when TraceConfigureLastBranchRecord succeeds |
| Context registers | Best-effort TraceContextRegisterInfo; payload decoding requires platform schema validation |
| Private providers | `record --provider GUID` |
| Disassembly | Zydis x86/x64, enabled by default |
| Release | Single EXE plus optional PDB/docs; no WPA/WPR runtime |

## Explicit limitations

- Linux `perf_event_open` group/count semantics cannot be reproduced exactly
  through ETW.
- Syscall argument-level interpretation is not provided.
- LBR depth is finite and architecture-dependent; it is not Intel PT.
- Context-register payload decoding is disabled until a stable schema is
  verified on supported platforms.
- 32-bit/WOW64 ETL replay must not crash, but full symbolization coverage is
  not claimed.
