[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Wperf,
    [Parameter(Mandatory = $true)][string]$Fixture,
    [Parameter(Mandatory = $true)][string]$SymbolPath,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [switch]$OffCpu
)

$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$name = if ($OffCpu) { "offcpu" } else { "cpu" }
$html = Join-Path $OutputDirectory "report-$name.html"
$arguments = @("report", "-i", $Fixture, "--symbol-path", $SymbolPath, "--quiet",
               "--flamegraph", "--title", "Fixture $name", "--countname", "custom-units")
$pidPath = "$Fixture.pid"
if (Test-Path -LiteralPath $pidPath) {
    $fixturePid = (Get-Content -LiteralPath $pidPath -Raw).Trim()
    $arguments += @("-p", $fixturePid)
}
if ($OffCpu) { $arguments += "--off-cpu" }
$arguments += @("-o", $html)

& $Wperf @arguments
if ($LASTEXITCODE -ne 0) { throw "report --flamegraph failed with exit $LASTEXITCODE" }
if (-not (Test-Path -LiteralPath $html)) { throw "report did not create $html" }
$text = Get-Content -LiteralPath $html -Raw
if (-not $text.Contains('id="flame-data"') -or
    -not $text.Contains('"weight_name":"custom-units"')) {
    throw "report HTML is missing embedded flame graph data or countname override"
}
if ($OffCpu) {
    if (-not $text.Contains('wperf off-CPU flame graph') -and
        -not $text.Contains('Fixture offcpu')) {
        throw "off-CPU flame graph title is wrong"
    }
    if (-not $text.Contains('blocked intervals')) { throw "off-CPU details are missing" }
} else {
    if (-not $text.Contains('Fixture cpu')) { throw "CPU flame graph title is wrong" }
    if (-not $text.Contains('weight basis')) { throw "CPU weight details are missing" }
}
Write-Output "report --flamegraph ${name}: PASS"
