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
foreach ($scriptContract in @(
    'state.navigation.slice(0,-1)',
    'state.selectedNode.valueText',
    'exactWeightText',
    'approximateWeightText'
)) {
    if (-not $text.Contains($scriptContract)) {
        throw "flamegraph JavaScript is missing contract: $scriptContract"
    }
}
if ($text.Contains('Math.round(state.selectedNode.value)')) {
    throw "copy folded must not round the JavaScript Number weight"
}

$maxWeight = '18446744073709551615'
$maxFolded = Join-Path $OutputDirectory "flamegraph-uint64.folded"
$maxHtml = Join-Path $OutputDirectory "flamegraph-uint64.html"
"app;main $maxWeight" | Set-Content -LiteralPath $maxFolded -Encoding utf8NoBOM
& $Wperf flamegraph -i $maxFolded -o $maxHtml
if ($LASTEXITCODE -ne 0) { throw "uint64 flamegraph failed with exit $LASTEXITCODE" }
$maxText = Get-Content -LiteralPath $maxHtml -Raw
if (-not $maxText.Contains('"value":"18446744073709551615"') -or
    $maxText.Contains('"value":18446744073709551615')) {
    throw "flamegraph HTML did not preserve uint64 weights as decimal strings"
}

$depth4096 = (0..4095 | ForEach-Object { "frame$_" }) -join ';'
$depth4097 = (0..4096 | ForEach-Object { "frame$_" }) -join ';'
$depthMaxFolded = Join-Path $OutputDirectory "flamegraph-depth-4096.folded"
$depthOverFolded = Join-Path $OutputDirectory "flamegraph-depth-4097.folded"
$depthMaxHtml = Join-Path $OutputDirectory "flamegraph-depth-4096.html"
"$depth4096 1" | Set-Content -LiteralPath $depthMaxFolded -Encoding utf8NoBOM
"$depth4097 1" | Set-Content -LiteralPath $depthOverFolded -Encoding utf8NoBOM
& $Wperf flamegraph -i $depthMaxFolded -o $depthMaxHtml
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $depthMaxHtml)) {
    throw "4096-level flamegraph should succeed, got exit $LASTEXITCODE"
}
& $Wperf flamegraph -i $depthOverFolded -o (Join-Path $OutputDirectory "flamegraph-depth-4097.html") 2>$null
if ($LASTEXITCODE -ne 6) {
    throw "4097-level flamegraph should return depth error 6, got $LASTEXITCODE"
}

$badFolded = Join-Path $OutputDirectory "flamegraph-invalid.folded"
"broken line`n" | Set-Content -LiteralPath $badFolded -Encoding utf8NoBOM
& $Wperf flamegraph -i $badFolded -o (Join-Path $OutputDirectory "invalid.html") 2>$null
if ($LASTEXITCODE -ne 6) { throw "malformed folded input should exit 6, got $LASTEXITCODE" }

Write-Output "flamegraph output contract: PASS"
