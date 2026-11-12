[CmdletBinding()]
param(
    [int]$DurationMs = 3000,
    [switch]$SkipRecord
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$build = Join-Path $repo "build"
$wperf = Join-Path $build "Release\wperf.exe"
$workload = Join-Path $build "Release\wperf_cpu_burn.exe"
$artifactDir = Join-Path $repo "artifacts\m2"
$trace = Join-Path $artifactDir "cpu.etl"
$cmake = "C:\Program Files\CMake\bin\cmake.exe"

if (-not (Test-Path $wperf)) {
    & $cmake -S $repo -B $build -G "Visual Studio 17 2022" -A x64
    if ($LASTEXITCODE -ne 0) { throw "configure failed" }
    & $cmake --build $build --config Release --parallel
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
}
New-Item -ItemType Directory -Force -Path $artifactDir | Out-Null

& $wperf report --help
if ($LASTEXITCODE -ne 0) { throw "report help failed" }

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
$elevated = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if ($SkipRecord) {
    Write-Host "Skipping record/report checks."
    Write-Host "M2/M3 non-record checks: PASS"
    exit 0
}

if (-not $elevated) {
    & $wperf record -F 999 -o $trace -- $workload $DurationMs 4
    if ($LASTEXITCODE -ne 3) {
        throw "unelevated record expected 3, got $LASTEXITCODE"
    }
    Write-Host "M2/M3 smoke requires an elevated terminal for real ETL capture."
    Write-Host "M2/M3 non-elevated checks: PASS"
    exit 0
}

Remove-Item -LiteralPath $trace -Force -ErrorAction SilentlyContinue
& $wperf record -F 999 -o $trace -- $workload $DurationMs 4
if ($LASTEXITCODE -ne 0) { throw "record failed with $LASTEXITCODE" }
if (-not (Test-Path $trace)) { throw "record did not create $trace" }

& $wperf report -i $trace --symbol-path (Join-Path $build 'Release') --sort samples |
    Out-File -LiteralPath (Join-Path $artifactDir "report.txt") -Encoding utf8
if ($LASTEXITCODE -ne 0) { throw "report failed with $LASTEXITCODE" }

& $wperf report -i $trace --symbol-path (Join-Path $build 'Release') --folded |
    Out-File -LiteralPath (Join-Path $artifactDir "stacks.folded") -Encoding utf8
if ($LASTEXITCODE -ne 0) { throw "folded report failed with $LASTEXITCODE" }

$attachTrace = Join-Path $artifactDir "attach.etl"
Remove-Item -LiteralPath $attachTrace -Force -ErrorAction SilentlyContinue
$target = Start-Process -FilePath $workload -ArgumentList @("3000", "2") -WindowStyle Hidden -PassThru
try {
    & $wperf record -p $target.Id --duration 1000 -o $attachTrace
    if ($LASTEXITCODE -ne 0) { throw "attach record failed with $LASTEXITCODE" }
} finally {
    if (-not $target.HasExited) {
        $target.WaitForExit(5000) | Out-Null
    }
}
& $wperf report -i $attachTrace --symbol-path (Join-Path $build 'Release') --percent-limit 1 |
    Out-File -LiteralPath (Join-Path $artifactDir "attach-report.txt") -Encoding utf8
if ($LASTEXITCODE -ne 0) { throw "attach report failed with $LASTEXITCODE" }

Write-Host "M2/M3 smoke: PASS"
Write-Host "  trace: $trace"
Write-Host "  report: $(Join-Path $artifactDir 'report.txt')"
Write-Host "  folded: $(Join-Path $artifactDir 'stacks.folded')"
