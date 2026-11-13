[CmdletBinding()]
param(
    [switch]$SkipRecord
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$build = Join-Path $repo "build"
$wperf = Join-Path $build "Release\wperf.exe"
$workload = Join-Path $build "Release\wperf_cpu_burn.exe"
$artifactDir = Join-Path $repo "artifacts\m6"
$cmake = "C:\Program Files\CMake\bin\cmake.exe"

if (-not (Test-Path $wperf)) {
    & $cmake -S $repo -B $build -G "Visual Studio 17 2022" -A x64
    if ($LASTEXITCODE -ne 0) { throw "configure failed" }
    & $cmake --build $build --config Release --parallel
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
}
New-Item -ItemType Directory -Force -Path $artifactDir | Out-Null

& $wperf list
if ($LASTEXITCODE -ne 0) { throw "list failed" }
& $wperf record --help
if ($LASTEXITCODE -ne 0) { throw "record help failed" }

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
$elevated = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if ($SkipRecord) {
    Write-Host "Skipping PMU/LBR capture."
    Write-Host "M6 non-capture checks: PASS"
    exit 0
}
if (-not $elevated) {
    Write-Host "M6 PMU/LBR capture requires an elevated terminal."
    exit 0
}

$listText = & $wperf list
$source = $null
$match = [regex]::Match(($listText -join "`n"), '(?m)^\s*(\d+)\s+\d+\s+\d+\s+TotalCycles\s*$')
if ($match.Success) {
    $source = [uint32]$match.Groups[1].Value
}
if ($null -eq $source) {
    Write-Host 'SKIP: TotalCycles PMC source is not available on this platform.'
    exit 0
}
Write-Host "Using PMC source $source"

$pmuTrace = Join-Path $artifactDir "pmu.etl"
Remove-Item -LiteralPath $pmuTrace -Force -ErrorAction SilentlyContinue
& $wperf record --profile pmu --pmc-source $source -o $pmuTrace -- $workload 1500 2
$pmuExit = $LASTEXITCODE
if ($pmuExit -eq 0) {
    & $wperf report -i $pmuTrace --symbol-path (Join-Path $build 'Release') --percent-limit 1 |
        Out-File -LiteralPath (Join-Path $artifactDir "pmu-report.txt") -Encoding utf8
    if ($LASTEXITCODE -ne 0) { throw "PMU replay failed" }
} else {
    "PMU capture failed/skipped with exit $pmuExit" |
        Out-File -LiteralPath (Join-Path $artifactDir "pmu-status.txt") -Encoding utf8
}

& $wperf stat --pmu -e TotalCycles --duration 1200 -- $workload 1000 2 |
    Out-File -LiteralPath (Join-Path $artifactDir "pmu-stat.txt") -Encoding utf8
$pmuStatExit = $LASTEXITCODE
if ($pmuStatExit -ne 0) {
    "PMU stat failed/skipped with exit $pmuStatExit" |
        Out-File -LiteralPath (Join-Path $artifactDir "pmu-stat-status.txt") -Encoding utf8
}

$lbrTrace = Join-Path $artifactDir "lbr.etl"
Remove-Item -LiteralPath $lbrTrace -Force -ErrorAction SilentlyContinue
$lbrOutput = & $wperf record --profile cpu --lbr -F 999 -o $lbrTrace -- $workload 800 1 2>&1
$lbrOutput | Out-File -LiteralPath (Join-Path $artifactDir 'lbr-capture.txt') -Encoding utf8
$lbrExit = $LASTEXITCODE
if ($lbrExit -ne 0) {
    "LBR unsupported or failed with exit $lbrExit" |
        Out-File -LiteralPath (Join-Path $artifactDir "lbr-status.txt") -Encoding utf8
}

if ($pmuExit -ne 0 -or $pmuStatExit -ne 0) {
    throw "PMC source was advertised, but capture/stat did not produce usable PMC samples"
}
if ($lbrExit -ne 0) {
    if (($lbrOutput -join "`n") -notmatch 'TraceConfigureLastBranchRecord.*\((50|1|120)\)') {
        throw "LBR capture failed without a documented unsupported capability: $lbrExit"
    }
    Write-Host "SKIP: native LBR API reports unsupported; PMC checks passed."
} else {
    $branches = & $wperf trace -i $lbrTrace -e lbr --max 1 --json | ConvertFrom-Json
    if ($LASTEXITCODE -ne 0 -or $branches.Count -eq 0) { throw 'LBR enabled without decoded branches' }
}
Write-Host "M6 PMC checks: PASS"
Write-Host "  PMU exit: $pmuExit, PMU stat exit: $pmuStatExit, LBR exit: $lbrExit"
