[CmdletBinding()]
param(
    [switch]$SkipRecord
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$build = Join-Path $repo "build"
$wperf = Join-Path $build "Release\wperf.exe"
$workload = Join-Path $build "Release\wperf_cpu_burn.exe"
$artifactDir = Join-Path $repo "artifacts\m4"
$cmake = "C:\Program Files\CMake\bin\cmake.exe"

if (-not (Test-Path $wperf)) {
    & $cmake -S $repo -B $build -G "Visual Studio 17 2022" -A x64
    if ($LASTEXITCODE -ne 0) { throw "configure failed" }
    & $cmake --build $build --config Release --parallel
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
}
New-Item -ItemType Directory -Force -Path $artifactDir | Out-Null

& $wperf top --help
if ($LASTEXITCODE -ne 0) { throw "top help failed" }
& $wperf stat --help
if ($LASTEXITCODE -ne 0) { throw "stat help failed" }
& $wperf script --help
if ($LASTEXITCODE -ne 0) { throw "script help failed" }

& $wperf stat --duration 300 -- $workload 250 2 |
    Out-File -LiteralPath (Join-Path $artifactDir "stat.txt") -Encoding utf8
if ($LASTEXITCODE -ne 0) { throw "stat failed with $LASTEXITCODE" }

if ($SkipRecord) {
    Write-Host "Skipping top capture."
    Write-Host "M4 checks: PASS"
    exit 0
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
$elevated = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $elevated) {
    Write-Host "top capture requires an elevated terminal; stat/script help checks passed."
    exit 0
}

& $wperf top --interval 1000 --iterations 1 |
    Out-File -LiteralPath (Join-Path $artifactDir "top.txt") -Encoding utf8
if ($LASTEXITCODE -ne 0) { throw "top failed with $LASTEXITCODE" }
Write-Host "M4 checks: PASS"
