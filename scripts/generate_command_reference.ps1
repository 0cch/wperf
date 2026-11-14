[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Wperf,
    [Parameter(Mandatory = $true)][string]$OutputPath
)

$ErrorActionPreference = "Stop"
$output = & $Wperf help --format markdown
if ($LASTEXITCODE -ne 0) {
    throw "wperf help --format markdown failed with exit code $LASTEXITCODE"
}
$text = (($output | ForEach-Object { $_.ToString() }) -join "`n").TrimEnd() + "`n"
$directory = Split-Path -Parent $OutputPath
if ($directory) {
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
}
[IO.File]::WriteAllText(
    $OutputPath,
    $text,
    [Text.UTF8Encoding]::new($false))
Write-Output "Generated $OutputPath"
