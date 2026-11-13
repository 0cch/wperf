[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$Wperf,
    [Parameter(Mandatory)] [string]$Fixture,
    [Parameter(Mandatory)] [string]$SymbolPath,
    [Parameter(Mandatory)] [string]$OutputDirectory
)

$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$output = Join-Path $OutputDirectory "folded-output.folded"
& $Wperf report -i $Fixture --symbol-path $SymbolPath --folded --quiet -o $output
if ($LASTEXITCODE -ne 0) {
    throw "folded report failed with exit $LASTEXITCODE"
}
if (-not (Test-Path -LiteralPath $output)) {
    throw "folded report did not create $output"
}

$bytes = [IO.File]::ReadAllBytes($output)
$text = [Text.Encoding]::UTF8.GetString($bytes)
if ($bytes.Length -eq 0) {
    throw "folded report is empty"
}
if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
    throw "folded report contains a UTF-8 BOM"
}
if ($text.Contains("`r")) {
    throw "folded report contains CR characters"
}
$lines = @($text -split "`n" | Where-Object { $_.Length -ne 0 })
if ($lines.Count -eq 0) {
    throw "folded report contains no stacks"
}
foreach ($line in $lines) {
    if ($line -notmatch '^.+ [0-9]+$') {
        throw "invalid folded stack line: $line"
    }
}
Write-Output "folded output contract: PASS ($($lines.Count) stacks, LF-only)"