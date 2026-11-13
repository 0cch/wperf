[CmdletBinding()]
param(
    [string]$Configuration = "Release",
    [string]$OutputDirectory = ""
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$build = Join-Path $repo "build"
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $repo "dist"
}
$cmake = "C:\Program Files\CMake\bin\cmake.exe"
$ctest = "C:\Program Files\CMake\bin\ctest.exe"

& $cmake -S $repo -B $build -G "Visual Studio 17 2022" -A x64
if ($LASTEXITCODE -ne 0) { throw "configure failed" }
& $cmake --build $build --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { throw "build failed" }
& $ctest --test-dir $build -C $Configuration --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "tests failed" }

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$sourceExe = Join-Path $build "$Configuration\wperf.exe"
$outputExe = Join-Path $OutputDirectory "wperf.exe"
Copy-Item -LiteralPath $sourceExe -Destination $outputExe -Force

$sourcePdb = Join-Path $build "$Configuration\wperf.pdb"
if (Test-Path $sourcePdb) {
    Copy-Item -LiteralPath $sourcePdb -Destination (Join-Path $OutputDirectory "wperf.pdb") -Force
}
foreach ($doc in @("README.md", "LICENSE")) {
    $source = Join-Path $repo $doc
    if (Test-Path $source) {
        Copy-Item -LiteralPath $source -Destination (Join-Path $OutputDirectory $doc) -Force
    }
}
$docsOutput = Join-Path $OutputDirectory "docs"
New-Item -ItemType Directory -Force -Path $docsOutput | Out-Null
foreach ($doc in @("PLAN.md", "IMPROVEMENT_PLAN.md", "R0_STATUS.md", "R1_STATUS.md",
                  "R2_STATUS.md", "R3_STATUS.md", "R4_STATUS.md", "R5_STATUS.md", "DISASSEMBLY_DECISION.md")) {
    $source = Join-Path $repo (Join-Path "docs" $doc)
    if (Test-Path $source) {
        Copy-Item -LiteralPath $source -Destination (Join-Path $docsOutput $doc) -Force
    }
}

$version = (& $outputExe --version) -join " "
$commit = (git -C $repo rev-parse HEAD).Trim()
$builtAt = (Get-Date).ToString('o')
$files = @()
foreach ($file in Get-ChildItem $OutputDirectory -File | Sort-Object Name) {
    $files += [ordered]@{
        name = $file.Name
        size = $file.Length
        sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $file.FullName).Hash
    }
}
$manifest = [ordered]@{
    product = "wperf"
    version = $version
    configuration = $Configuration
    git_commit = $commit
    built_at = $builtAt
    architecture = "x64"
    runtime_dependency = "Windows system DLLs only; no WPA/WPR runtime"
    files = $files
}
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $OutputDirectory "manifest.json") -Encoding utf8

$compatibility = @"
# wperf compatibility

- Target: Windows 10 21H2+ and Windows 11 x64.
- Capture: ETW system logger and private provider sessions.
- Symbols: DbgHelp, local PDB, and optionally configured symbol server.
- PMU/LBR/context-register availability is platform and privilege dependent.
- Unsupported hardware features are reported explicitly; they are not synthesized.
- The release is a single CLI executable plus optional PDB/docs; no WPA/WPR runtime is required.
"@
Set-Content -LiteralPath (Join-Path $OutputDirectory "COMPATIBILITY.md") -Value $compatibility -Encoding utf8

@(
    "wperf release",
    "configuration=$Configuration",
    "version=$version",
    "git_commit=$commit",
    "built_at=$builtAt",
    "manifest=manifest.json",
    "compatibility=COMPATIBILITY.md"
) | Set-Content -LiteralPath (Join-Path $OutputDirectory "RELEASE.txt") -Encoding utf8
Write-Host "Release output: $outputExe"
Write-Host "Version: $version"
Write-Host "Commit: $commit"
Write-Host "Manifest: $(Join-Path $OutputDirectory 'manifest.json')"