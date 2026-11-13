# Release

```powershell
pwsh -NoProfile -File .\scripts\build_release.ps1
```

The release directory contains:

- `wperf.exe`
- `wperf.pdb`
- `README.md`, `CHANGELOG.md`, notices and final docs
- `manifest.json` with version, git commit, file sizes and SHA256 values
- `COMPATIBILITY.md`
- `RELEASE.txt`
- Zydis/Zycore notices when disassembly is enabled

The executable depends only on Windows system DLLs and does not require
WPA/WPR. Zydis is statically linked. The PDB is optional for execution and is
provided for crash diagnostics.
