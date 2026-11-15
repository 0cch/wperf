[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Wperf)
$ErrorActionPreference='Stop'
$jsonText = & $Wperf stat --duration 1 --json -- cmd.exe /d /c "exit 0" 2>$null
if ($LASTEXITCODE -ne 0) { throw "stat --json failed: $LASTEXITCODE" }
$row = (($jsonText -join "`n") | ConvertFrom-Json)
foreach ($field in @('source','target_pid','target_exit_code','elapsed_ms','user_ms','sys_ms','cycles','page_faults','io_read_ops','io_read_bytes','io_write_ops','io_write_bytes')) {
    if (-not $row.PSObject.Properties.Name.Contains($field)) { throw "stat JSON missing $field" }
}
if ($row.source -ne 'windows-api') { throw "unexpected stat source: $($row.source)" }
$csvText = & $Wperf stat --duration 1 --csv -- cmd.exe /d /c "exit 0" 2>$null
if ($LASTEXITCODE -ne 0) { throw "stat --csv failed: $LASTEXITCODE" }
if ($csvText[0] -ne 'elapsed_ms,user_ms,sys_ms,cycles,page_faults,io_read_ops,io_read_bytes,io_write_ops,io_write_bytes,target_exit_code') { throw "unexpected stat CSV header: $($csvText[0])" }
$intervalText = & $Wperf stat --duration 1200 --interval 200 --json -- cmd.exe /d /c "ping -n 2 127.0.0.1 >nul" 2>$null
if ($LASTEXITCODE -ne 0) { throw "stat --interval failed: $LASTEXITCODE" }
$interval = (($intervalText -join "`n") | ConvertFrom-Json)
if (@($interval.samples).Count -lt 2) { throw "stat interval produced too few samples" }
Write-Output 'stat output: PASS'
