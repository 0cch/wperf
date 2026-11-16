[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Wperf,
    [Parameter(Mandatory = $true)][string]$Fixture,
    [Parameter(Mandatory = $true)][string]$Command,
    [switch]$ApplyPid,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$ToolArguments
)

$ErrorActionPreference = 'Stop'
$arguments = @($Command, '-i', $Fixture)
if ($ApplyPid) {
    $pidPath = $Fixture + '.pid'
    if (-not (Test-Path -LiteralPath $pidPath -PathType Leaf)) {
        throw "Missing generated fixture PID file: $pidPath"
    }
    $fixturePid = (Get-Content -LiteralPath $pidPath -Raw).Trim()
    if ($fixturePid -notmatch '^\d+$') {
        throw "Invalid generated fixture PID: $fixturePid"
    }
    $arguments += @('-p', $fixturePid)
}
if ($ToolArguments) {
    $arguments += $ToolArguments
}
& $Wperf @arguments
exit $LASTEXITCODE
