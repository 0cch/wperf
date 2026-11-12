[CmdletBinding()]
param(
    [switch]$SkipRecord
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$build = Join-Path $repo "build"
$wperf = Join-Path $build "Release\wperf.exe"
$workload = Join-Path $build "Release\wperf_cpu_burn.exe"
$artifactDir = Join-Path $repo "artifacts\m5"
$cmake = "C:\Program Files\CMake\bin\cmake.exe"

if (-not (Test-Path $wperf)) {
    & $cmake -S $repo -B $build -G "Visual Studio 17 2022" -A x64
    if ($LASTEXITCODE -ne 0) { throw "configure failed" }
    & $cmake --build $build --config Release --parallel
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
}
New-Item -ItemType Directory -Force -Path $artifactDir | Out-Null

& $wperf trace --help
if ($LASTEXITCODE -ne 0) { throw "trace help failed" }
& $wperf sched --help
if ($LASTEXITCODE -ne 0) { throw "sched help failed" }

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
$elevated = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if ($SkipRecord) {
    Write-Host "Skipping M5 record/report checks."
    Write-Host "M5 non-record checks: PASS"
    exit 0
}
if (-not $elevated) {
    Write-Host "M5 smoke requires an elevated terminal for scheduler/pagefault capture."
    Write-Host "M5 non-elevated checks: PASS"
    exit 0
}

$schedTrace = Join-Path $artifactDir "sched.etl"
$removedPaths = @($schedTrace, (Join-Path $artifactDir "trace.txt"), (Join-Path $artifactDir "sched.txt"))
foreach ($path in $removedPaths) {
    Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
}
& $wperf record --profile cpu-sched -F 999 -o $schedTrace -- $workload 2500 4
if ($LASTEXITCODE -ne 0) { throw "sched record failed with $LASTEXITCODE" }
& $wperf trace -i $schedTrace -e sched --max 30 |
    Out-File -LiteralPath (Join-Path $artifactDir "trace.txt") -Encoding utf8
if ($LASTEXITCODE -ne 0) { throw "trace failed with $LASTEXITCODE" }
& $wperf sched -i $schedTrace |
    Out-File -LiteralPath (Join-Path $artifactDir "sched.txt") -Encoding utf8
if ($LASTEXITCODE -ne 0) { throw "sched report failed with $LASTEXITCODE" }

$vmTrace = Join-Path $artifactDir "vm.etl"
Remove-Item -LiteralPath $vmTrace -Force -ErrorAction SilentlyContinue
& $wperf record --profile vm -F 999 -o $vmTrace -- $workload 2000 2
if ($LASTEXITCODE -ne 0) { throw "vm record failed with $LASTEXITCODE" }
& $wperf trace -i $vmTrace -e pagefault --max 30 |
    Out-File -LiteralPath (Join-Path $artifactDir "pagefault.txt") -Encoding utf8
if ($LASTEXITCODE -ne 0) { throw "pagefault trace failed with $LASTEXITCODE" }

$syscallTrace = Join-Path $artifactDir "syscall.etl"
Remove-Item -LiteralPath $syscallTrace -Force -ErrorAction SilentlyContinue
& $wperf record --profile syscall -F 999 -o $syscallTrace -- $workload 1200 2
if ($LASTEXITCODE -ne 0) { throw "syscall record failed with $LASTEXITCODE" }
& $wperf trace -i $syscallTrace -e syscall --max 20 |
    Out-File -LiteralPath (Join-Path $artifactDir "syscall.txt") -Encoding utf8
if ($LASTEXITCODE -ne 0) { throw "syscall trace failed with $LASTEXITCODE" }

$ioTrace = Join-Path $artifactDir "io.etl"
Remove-Item -LiteralPath $ioTrace -Force -ErrorAction SilentlyContinue
& $wperf record --profile io -F 999 -o $ioTrace -- $workload 1200 2
if ($LASTEXITCODE -ne 0) { throw "io record failed with $LASTEXITCODE" }
& $wperf trace -i $ioTrace -e io --max 20 |
    Out-File -LiteralPath (Join-Path $artifactDir "io.txt") -Encoding utf8
if ($LASTEXITCODE -ne 0) { throw "io trace failed with $LASTEXITCODE" }

Write-Host "M5 smoke: PASS"
Write-Host "  sched trace: $schedTrace"
Write-Host "  vm trace: $vmTrace"
