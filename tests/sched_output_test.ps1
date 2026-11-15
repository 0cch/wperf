[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Wperf,[Parameter(Mandatory=$true)][string]$Fixture)
$ErrorActionPreference='Stop'
$jsonText = & $Wperf sched -i $Fixture --json 2>$null
if ($LASTEXITCODE -ne 0) { throw "sched --json failed: $LASTEXITCODE" }
$doc = (($jsonText -join "`n") | ConvertFrom-Json)
if ($null -eq $doc.quality -or $null -eq $doc.rows) { throw 'sched JSON must contain quality and rows' }
if (@($doc.rows).Count -eq 0) { throw 'sched returned no rows' }
foreach ($field in @('pid','comm','running_ms','ready_ms','blocked_ms','wakeups','wakeup_avg_us','wakeup_max_us','p50_us','p95_us','p99_us','top_wait_reason')) {
    if (-not $doc.rows[0].PSObject.Properties.Name.Contains($field)) { throw "sched JSON missing $field" }
}
$csvText = & $Wperf sched -i $Fixture --csv 2>$null
if ($LASTEXITCODE -ne 0) { throw "sched --csv failed: $LASTEXITCODE" }
if ($csvText[0] -ne 'pid,comm,running_ms,ready_ms,blocked_ms,wakeups,wakeup_avg_us,wakeup_max_us,p50_us,p95_us,p99_us,top_wait_reason') { throw "unexpected sched CSV header: $($csvText[0])" }
Write-Output 'sched output: PASS'
