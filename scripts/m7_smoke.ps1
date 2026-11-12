[CmdletBinding()]
param(
    [switch]$SkipRecord
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$wperf = Join-Path $repo "build\Release\wperf.exe"
$cmake = "C:\Program Files\CMake\bin\cmake.exe"

if (-not (Test-Path $wperf)) {
    & $cmake -S $repo -B (Join-Path $repo "build") -G "Visual Studio 17 2022" -A x64
    if ($LASTEXITCODE -ne 0) { throw "configure failed" }
    & $cmake --build (Join-Path $repo "build") --config Release --parallel
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
}

& $wperf annotate --help
if ($LASTEXITCODE -ne 0) { throw "annotate help failed" }
& $wperf diff --help
if ($LASTEXITCODE -ne 0) { throw "diff help failed" }
& $wperf archive --help
if ($LASTEXITCODE -ne 0) { throw "archive help failed" }
& $wperf buildid-list --help
if ($LASTEXITCODE -ne 0) { throw "buildid-list help failed" }

$m2Trace = Join-Path $repo "artifacts\m2\cpu.etl"
if ((Test-Path $m2Trace) -and -not $SkipRecord) {
    $artifactDir = Join-Path $repo "artifacts\m7"
    New-Item -ItemType Directory -Force -Path $artifactDir | Out-Null
    & $wperf annotate -i $m2Trace --symbol-path (Join-Path $repo "build\Release") --limit 20 |
        Out-File -LiteralPath (Join-Path $artifactDir "annotate.txt") -Encoding utf8
    if ($LASTEXITCODE -ne 0) { throw "annotate failed" }
    & $wperf diff -1 $m2Trace -2 $m2Trace --symbol-path (Join-Path $repo "build\Release") |
        Out-File -LiteralPath (Join-Path $artifactDir "diff.txt") -Encoding utf8
    if ($LASTEXITCODE -ne 0) { throw "diff failed" }
} else {
    Write-Host "Skipping M7 trace-driven checks."
}

Write-Host "M7 checks: PASS"
