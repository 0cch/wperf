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

FlameGraph folded data is written directly by `wperf report --folded -o FILE`;
SVG rendering uses the external `flamegraph.pl` and Perl.

## Tag releases

Pushing a tag matching `vMAJOR.MINOR.PATCH` builds and publishes
`vMAJOR.MINOR.PATCH.GITHUB_RUN_NUMBER`. For example, tag `v1.2.3` produces
release version `v1.2.3.42` when the workflow run number is 42. A full
four-part tag such as `v1.2.3.42` is also accepted as an explicit override.

The release workflow publishes the EXE, PDB, manifest, zip archive and SHA256
checksums to GitHub Releases.
