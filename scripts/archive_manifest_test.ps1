param(
    [Parameter(Mandatory=$true)][string]$Wperf,
    [Parameter(Mandatory=$true)][string]$Fixture,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [Parameter(Mandatory=$true)][string]$SchemaPath
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
$manifestText = Get-Content (Join-Path $output 'manifest.json') -Raw
if (-not (Test-Json -Json $manifestText -SchemaFile $SchemaPath -ErrorAction Stop)) { throw 'archive manifest does not match schema' }
$manifest = $manifestText | ConvertFrom-Json
Write-Output "archive manifest test: PASS ($($manifest.entries.Count) modules)"
