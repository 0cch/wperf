[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Wperf,[Parameter(Mandatory=$true)][string]$Fixture)
$ErrorActionPreference='Stop'
$out = & $Wperf diff -1 $Fixture -2 $Fixture --normalize weight --first-pid 999999 --second-pid 999999 --json 2>$null
if ($LASTEXITCODE -ne 0) { throw "diff normalize options failed: $LASTEXITCODE" }
$text = $out -join "`n"
if (-not $text.Trim().StartsWith("[")) { throw "diff JSON did not parse" }
Write-Output 'diff options: PASS'
