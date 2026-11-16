[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Wperf,[Parameter(Mandatory=$true)][string]$Fixture)
$ErrorActionPreference='Stop'
$fixturePid=(Get-Content -LiteralPath ($Fixture + ".pid") -Raw).Trim()
$out = & $Wperf diff -1 $Fixture -2 $Fixture --normalize weight --first-pid $fixturePid --second-pid $fixturePid --json 2>$null
if ($LASTEXITCODE -ne 0) { throw "diff normalize options failed: $LASTEXITCODE" }
$text = $out -join "`n"
if (-not $text.Trim().StartsWith("[")) { throw "diff JSON did not parse" }
Write-Output 'diff options: PASS'
