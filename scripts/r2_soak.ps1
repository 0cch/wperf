param(
    [int]$DurationMinutes = 60,
    [int]$IntervalMs = 1000,
    [string]$Wperf = (Join-Path $PSScriptRoot '..\build\Release\wperf.exe')
)

$ErrorActionPreference = 'Stop'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host 'R2 soak requires an elevated PowerShell terminal.'
    exit 2
}
if (-not (Test-Path $Wperf)) {
    Write-Host "wperf.exe not found: $Wperf"
    exit 2
}

$iterations = [int][Math]::Max(1, $DurationMinutes * 60 * 1000 / $IntervalMs)
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$outputDir = Join-Path $PSScriptRoot "..\artifacts\r2-soak-$stamp"
New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
$stdout = Join-Path $outputDir 'top.stdout.txt'
$stderr = Join-Path $outputDir 'top.stderr.txt'
$metrics = Join-Path $outputDir 'process-metrics.csv'

"timestamp,role,pid,working_set,handle_count,cpu_seconds" | Set-Content -Path $metrics -Encoding utf8
$arguments = @('top', '--interval', "$IntervalMs", '--iterations', "$iterations")
$process = Start-Process -FilePath $Wperf -ArgumentList $arguments -PassThru `
    -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr
$started = Get-Date
$lastSample = [DateTime]::MinValue
while (-not $process.HasExited) {
    if (((Get-Date) - $lastSample).TotalSeconds -ge 30) {
        $current = Get-Process -Id $process.Id -ErrorAction SilentlyContinue
        if ($null -ne $current) {
            "$(Get-Date -Format o),controller,$($current.Id),$($current.WorkingSet64),$($current.HandleCount),$($current.CPU)" |
                Add-Content -Path $metrics -Encoding utf8
        }
        $workers = Get-CimInstance Win32_Process -Filter "ParentProcessId=$($process.Id)" |
            Where-Object { $_.Name -eq 'wperf.exe' -and $_.CommandLine -match '--internal-live-worker' }
        foreach ($worker in $workers) {
            $workerMetrics = Get-Process -Id $worker.ProcessId -ErrorAction SilentlyContinue
            if ($null -ne $workerMetrics) {
                "$(Get-Date -Format o),worker,$($workerMetrics.Id),$($workerMetrics.WorkingSet64),$($workerMetrics.HandleCount),$($workerMetrics.CPU)" |
                    Add-Content -LiteralPath $metrics -Encoding utf8
            }
        }
        $lastSample = Get-Date
    }
    Start-Sleep -Seconds 1
    $process.Refresh()
}
$elapsed = (Get-Date) - $started
$exitCode = $process.ExitCode
$sessions = & $Wperf list 2>&1 | Out-String
$leftover = $sessions -match 'wperf-top-'

$summary = [ordered]@{
    duration_minutes = $DurationMinutes
    interval_ms = $IntervalMs
    iterations = $iterations
    elapsed_seconds = [Math]::Round($elapsed.TotalSeconds, 2)
    exit_code = $exitCode
    leftover_top_session = [bool]$leftover
    output_directory = (Resolve-Path $outputDir).Path
    stdout_bytes = (Get-Item $stdout).Length
    stderr_bytes = (Get-Item $stderr).Length
}
$summary | ConvertTo-Json | Set-Content -Path (Join-Path $outputDir 'summary.json') -Encoding utf8
$summary | ConvertTo-Json

if ($exitCode -ne 0 -or $leftover) {
    exit 1
}
Write-Output 'R2 soak: PASS'