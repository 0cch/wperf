[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Wperf,
    [Parameter(Mandatory = $true)][string]$OutputDirectory
)

$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$folded = Join-Path $OutputDirectory "flamegraph-input.folded"
$html = Join-Path $OutputDirectory "flamegraph-output.html"
@"
app.exe[42@1];main;alpha 60
app.exe[42@1];main;beta 40
app.exe[42@1];main;alpha;<script> 5
"@ | Set-Content -LiteralPath $folded -Encoding utf8NoBOM

& $Wperf flamegraph -i $folded --countname nanoseconds --title "Smoke <Graph>" -o $html
if ($LASTEXITCODE -ne 0) { throw "flamegraph command failed with exit $LASTEXITCODE" }
if (-not (Test-Path -LiteralPath $html)) { throw "flamegraph output was not created" }
$bytes = [IO.File]::ReadAllBytes($html)
if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xef -and $bytes[1] -eq 0xbb -and $bytes[2] -eq 0xbf) {
    throw "flamegraph output contains a UTF-8 BOM"
}
if ($bytes -contains 0x0d) { throw "flamegraph output contains CR characters" }
$text = [Text.Encoding]::UTF8.GetString($bytes)
foreach ($required in @(
    '<!doctype html>',
    '<title>Smoke &lt;Graph&gt;</title>',
    'id="flame-data"',
    '"weight_name":"nanoseconds"',
    '\u003cscript\u003e',
    'id="min-percent"',
    'id="details-stack"',
    'id="copy-stack"',
    'id="copy-folded"',
    'id="details-empty"',
    'id="breadcrumb"',
    'id="download-svg"',
    'id="download-png"',
    'drag horizontally to select a range'
)) {
    if (-not $text.Contains($required)) { throw "flamegraph HTML is missing: $required" }
}
if ($text.Contains('https://')) { throw "flamegraph HTML contains an external URL" }
if ($text.Contains('class="primary"')) { throw "download buttons must use a consistent style" }

$badFolded = Join-Path $OutputDirectory "flamegraph-invalid.folded"
"broken line`n" | Set-Content -LiteralPath $badFolded -Encoding utf8NoBOM
& $Wperf flamegraph -i $badFolded -o (Join-Path $OutputDirectory "invalid.html") 2>$null
if ($LASTEXITCODE -ne 6) { throw "malformed folded input should exit 6, got $LASTEXITCODE" }

Write-Output "flamegraph output contract: PASS"
