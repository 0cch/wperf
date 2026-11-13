param(
    [Parameter(Mandatory=$true)][string]$Wperf,
    [Parameter(Mandatory=$true)][string]$Fixture,
    [Parameter(Mandatory=$true)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
$output = Join-Path $OutputDirectory ("archive-" + [guid]::NewGuid().ToString('N'))
& $Wperf archive -i $Fixture -o $output --max-size-mb 1
if ($LASTEXITCODE -ne 0) {
    throw "archive failed with exit code $LASTEXITCODE"
}
foreach ($name in @('manifest.json', 'manifest.csv', 'manifest.txt')) {
    if (-not (Test-Path (Join-Path $output $name))) {
        throw "missing $name"
    }
}
$manifest = Get-Content (Join-Path $output 'manifest.json') -Raw | ConvertFrom-Json
if ($manifest.entries.Count -eq 0) {
    throw 'archive manifest has no entries'
}
Write-Output "archive smoke: PASS ($($manifest.entries.Count) modules)"