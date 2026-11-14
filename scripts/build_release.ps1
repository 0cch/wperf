[CmdletBinding()]
param(
    [string]$Configuration = "Release",
    [string]$OutputDirectory = "",
    [string]$VersionMajor = "0",
    [string]$VersionMinor = "8",
    [string]$VersionPatch = "0",
    [string]$BuildNumber = "0"
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$build = Join-Path $repo "build"
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $repo "dist"
}
$cmake = (Get-Command cmake.exe -ErrorAction SilentlyContinue | Select-Object -First 1).Source
$ctest = (Get-Command ctest.exe -ErrorAction SilentlyContinue | Select-Object -First 1).Source
if ([string]::IsNullOrWhiteSpace($cmake)) {
    $cmake = "C:\Program Files\CMake\bin\cmake.exe"
}
if ([string]::IsNullOrWhiteSpace($ctest)) {
    $ctest = "C:\Program Files\CMake\bin\ctest.exe"
}
$fullVersion = "$VersionMajor.$VersionMinor.$VersionPatch.$BuildNumber"
Write-Host "Building wperf $fullVersion"

$configureArguments = @(
    "-S", $repo,
    "-B", $build,
    "-G", "Visual Studio 17 2022",
    "-A", "x64",
    "-DWPERF_VERSION_MAJOR=$VersionMajor",
    "-DWPERF_VERSION_MINOR=$VersionMinor",
    "-DWPERF_VERSION_PATCH=$VersionPatch",
    "-DWPERF_BUILD_NUMBER=$BuildNumber"
)
& $cmake @configureArguments
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
foreach ($doc in @("README.md", "CHANGELOG.md", "LICENSE", "THIRD_PARTY_NOTICES.md",
                  "CONTRIBUTING.md", "SECURITY.md")) {
    $source = Join-Path $repo $doc
    if (Test-Path $source) {
        Copy-Item -LiteralPath $source -Destination (Join-Path $OutputDirectory $doc) -Force
    }
}
$docsOutput = Join-Path $OutputDirectory "docs"
New-Item -ItemType Directory -Force -Path $docsOutput | Out-Null
foreach ($doc in @("USAGE.md", "ARCHITECTURE.md", "COMPATIBILITY.md",
                  "DEVELOPMENT.md", "PUBLIC_CONTRACT.md", "COMMAND_REFERENCE.md", "RELEASE.md",
                  "HARDWARE_VALIDATION.md")) {
    $source = Join-Path $repo (Join-Path "docs" $doc)
    if (Test-Path $source) {
        Copy-Item -LiteralPath $source -Destination (Join-Path $docsOutput $doc) -Force
    }
}

$schemaSource = Join-Path $repo "docs\schemas"
if (Test-Path $schemaSource) {
    Copy-Item -LiteralPath $schemaSource -Destination (Join-Path $docsOutput "schemas") -Recurse -Force
}

$versionOutput = (& $outputExe --version) -join " "
$version = $versionOutput -replace '^wperf\s+', ''
$commit = (git -C $repo rev-parse HEAD).Trim()
$builtAt = (Get-Date).ToString('o')


@(
    "wperf release",
    "configuration=$Configuration",
    "version=$version",
    "git_commit=$commit",
    "built_at=$builtAt",
    "manifest=manifest.json",
    "compatibility=docs/COMPATIBILITY.md"
) | Set-Content -LiteralPath (Join-Path $OutputDirectory "RELEASE.txt") -Encoding utf8

$files = @()
foreach ($file in Get-ChildItem $OutputDirectory -Recurse -File | Sort-Object FullName) {
    if ($file.Name -eq "manifest.json") {
        continue
    }
    $relative = [IO.Path]::GetRelativePath($OutputDirectory, $file.FullName).Replace('\', '/')
    $files += [ordered]@{
        name = $relative
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

Write-Host "Release output: $outputExe"
Write-Host "Version: $version"
Write-Host "Commit: $commit"
Write-Host "Manifest: $(Join-Path $OutputDirectory 'manifest.json')"
