[CmdletBinding()]
param(
    [switch]$SkipRecord
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$build = Join-Path $repo "build"
$wperf = Join-Path $build "Release\wperf.exe"
$cmake = "C:\Program Files\CMake\bin\cmake.exe"

if (-not (Test-Path $wperf)) {
    & $cmake -S $repo -B $build -G "Visual Studio 17 2022" -A x64
    if ($LASTEXITCODE -ne 0) { throw "configure failed" }
    & $cmake --build $build --config Release --parallel
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
}

function Assert-Exit {
    param([int]$Expected, [string]$Name)
    if ($LASTEXITCODE -ne $Expected) {
        throw "$Name expected exit $Expected, got $LASTEXITCODE"
    }
}

& $wperf --version
Assert-Exit 0 "version"
& $wperf list
Assert-Exit 0 "list"
& $wperf doctor
Assert-Exit 0 "doctor"
& $wperf test
Assert-Exit 0 "test"

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
$elevated = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not $SkipRecord) {
    if ($elevated) {
        $artifactDir = Join-Path $repo "artifacts\m1"
        New-Item -ItemType Directory -Force -Path $artifactDir | Out-Null
        $trace = Join-Path $artifactDir "smoke.etl"
        Remove-Item -LiteralPath $trace -Force -ErrorAction SilentlyContinue
        & $wperf record --duration 1000 -o $trace
        Assert-Exit 0 "elevated record"
        if (-not (Test-Path $trace)) {
            throw "elevated record did not create $trace"
        }
    } else {
        & $wperf record --duration 1000
        Assert-Exit 3 "unelevated record guard"
        Write-Host "Skipping end-to-end record: use an elevated terminal."
    }
} else {
    Write-Host "Skipping record checks."
}

Write-Host "M1 smoke: PASS"
