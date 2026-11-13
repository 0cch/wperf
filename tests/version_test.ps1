[CmdletBinding()]
param([Parameter(Mandatory)] [string]$Wperf)

$ErrorActionPreference = "Stop"
$version = (& $Wperf --version) -join " "
if ($LASTEXITCODE -ne 0) {
    throw "wperf --version failed with exit $LASTEXITCODE"
}
if ($version -notmatch '^wperf \d+\.\d+\.\d+\.\d+$') {
    throw "version does not use four-part form: $version"
}
Write-Output "version contract: PASS ($version)"