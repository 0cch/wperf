param(
    [Parameter(Mandatory = $true)][string]$Wperf
)

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Output "SKIP: live top controller smoke requires elevation"
    exit 77
}

$output = & $Wperf top --iterations 1 --interval 100 2>&1 | Out-String
$exitCode = $LASTEXITCODE
if ($exitCode -ne 0) {
    throw "wperf top failed with exit $exitCode`n$output"
}
if ($output -notmatch 'wperf top') {
    throw "wperf top did not render the controller header`n$output"
}
if ($output -notmatch 'samples=') {
    throw "wperf top did not render decoded window statistics`n$output"
}
Write-Output "live top controller decode/render smoke: PASS"
