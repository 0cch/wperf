[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Wperf,
    [Parameter(Mandatory=$true)][string]$Fixture
)

$ErrorActionPreference = "Stop"
$default = & $Wperf trace -i $Fixture -e sched --json 2>$null
if ($LASTEXITCODE -ne 0) { throw "default trace failed: $LASTEXITCODE" }
$events = (($default -join "`n") | ConvertFrom-Json)
if (@($events).Count -le 500) { throw "default trace returned only $(@($events).Count) events" }

$limited = & $Wperf trace -i $Fixture -e sched --json --max 10 2>&1
if ($LASTEXITCODE -ne 0) { throw "limited trace failed: $LASTEXITCODE" }
$text = $limited -join "`n"
$jsonStart = $text.IndexOf("[")
$jsonEnd = $text.LastIndexOf("]")
if ($jsonStart -lt 0 -or $jsonEnd -lt $jsonStart) { throw "limited trace did not contain JSON" }
$limitedEvents = ($text.Substring($jsonStart, $jsonEnd - $jsonStart + 1) | ConvertFrom-Json)
if (@($limitedEvents).Count -ne 10) { throw "expected 10 limited events, got $(@($limitedEvents).Count)" }
if ($text -notmatch "truncated") { throw "limited trace did not warn about truncation" }
$meta = & $Wperf trace -i $Fixture -e sched --json --metadata --max 10 2>$null
if ($LASTEXITCODE -ne 0) { throw "trace metadata failed: $LASTEXITCODE" }
$doc = (($meta -join "`n") | ConvertFrom-Json)
if ($null -eq $doc.quality -or -not $doc.truncated) { throw 'trace metadata did not report truncation' }
if (@($doc.events).Count -ne 10) { throw "trace metadata expected 10 events, got $(@($doc.events).Count)" }
Write-Output "trace limit: PASS"
