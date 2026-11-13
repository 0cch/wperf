# Disassembly

wperf uses Zydis v4.1.1 with the pinned Zycore revision
`0b2432ced0884fd152b471d97ecf0258ff4d859f`.

- Build option: `WPERF_ENABLE_DISASM` (default `ON`).
- Link mode: static.
- Architectures: x86/x64; current primary target is x64.
- No execution of target code and no driver/MSR access.

## Usage

```powershell
wperf annotate -i trace.etl --limit 20 --disasm
wperf annotate -i trace.etl --limit 20 --disasm --json
```

The hot module is mapped from PE RVA to file offset, instruction bytes are read
from the module file, and Zydis formats Intel-syntax instructions. Symbol and
source-line failures do not block instruction decoding.

## Fallback

With `-DWPERF_ENABLE_DISASM=OFF`, `annotate` remains available as address heat
and emits a warning when `--disasm` is requested. Unit tests conditionally skip
Zydis-specific assertions in this mode.
