# Release Process

## Versioning

wperf uses four numeric components:

```text
MAJOR.MINOR.PATCH.BUILD
```

The CMake defaults define the first three. Normal release tags must use exactly
three components, for example `v0.12.0`. The GitHub release workflow always
derives the fourth component from `GITHUB_RUN_NUMBER` and creates the final
four-part release tag.

Example:

- tag `v0.12.0` on workflow run 42 produces release `0.12.0.42`.

The workflow rejects manually pushed four-part release tags such as
`v0.12.0.42`; the fourth component is generated output, not an input. Re-running
the same workflow keeps the same `GITHUB_RUN_NUMBER`. Run numbers are monotonic
but not guaranteed to be contiguous.

Release tags are immutable once published. Do not reuse or force-move a tag.

## Local release build

```powershell
pwsh -NoProfile -File .\scripts\build_release.ps1
```

The script configures/builds the Release x64 target, runs CTest, and writes the
release directory to `dist/` by default.

Override the output or version when needed:

```powershell
pwsh -NoProfile -File .\scripts\build_release.ps1 `
  -OutputDirectory artifacts\release `
  -VersionMajor 0 -VersionMinor 12 -VersionPatch 0 -BuildNumber 0
```

## Release contents

Published GitHub Release assets:

- `wperf.exe`
- `wperf.pdb`
- `manifest.json`
- `SHA256SUMS.txt`

The release package also contains:

- `README.md`, `CHANGELOG.md`, `CONTRIBUTING.md`, `SECURITY.md`, and third-party notices;
- `docs/COMPATIBILITY.md`, `docs/USAGE.md`, `docs/ARCHITECTURE.md`,
  `docs/DEVELOPMENT.md`, `docs/PUBLIC_CONTRACT.md`, `docs/RELEASE.md`,
  `docs/HARDWARE_VALIDATION.md`, `docs/DIAGNOSTICS.md`, and `docs/schemas/`;
- `RELEASE.txt` with version/commit/build metadata.

## Tag-triggered GitHub release

The workflow is `.github/workflows/release.yml` and triggers on tags matching
`v*`.

```powershell
git tag -a v0.12.0 -m "wperf 0.12.0"
git push origin v0.12.0
```

The workflow:

1. validates the tag and derives the release version;
2. builds and tests Release x64;
3. generates SHA256 checksums for exe, PDB, and manifest;
4. uploads the workflow artifact;
5. creates a non-draft, non-prerelease GitHub Release with generated GitHub notes.

The live integration tests are required by the workflow. A skipped or
unsupported hardware step must not be treated as a pass.

## Pre-release checklist

- [ ] Changelog updated, version values reviewed, and the release-note body prepared locally.
- [ ] Normal CTest passes.
- [ ] Elevated live tests pass with `WPERF_REQUIRE_LIVE_TESTS=1`.
- [ ] Hardware-dependent claims have evidence in `docs/HARDWARE_VALIDATION.md`.
- [ ] README/USAGE/COMPATIBILITY reflect current behavior.
- [ ] No raw ETLs, dumps, secrets, or machine-specific paths are committed.
- [ ] Release notes identify known limitations and unverified platforms.
- [ ] `LICENSE` exists before the repository is made public.

## Update release notes with gh

The workflow creates the release automatically so that tags remain immutable
and no credentials are needed outside GitHub Actions. Replace the generated
notes with the reviewed release-note body after the workflow succeeds:

```powershell
gh release list --limit 5
gh release edit <release-tag> --notes-file .\out\release-notes-<version>.md
```

The note file is intentionally a local release artifact rather than a committed
repository file.

## Post-release verification

```powershell
gh release list --limit 5
gh run list --workflow release.yml --limit 5
```

Confirm the release is non-draft, required assets exist, and the manifest
records the expected commit and version.
