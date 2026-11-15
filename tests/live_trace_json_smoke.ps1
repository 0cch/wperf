[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Wperf
)

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Output "SKIP: live trace JSON smoke requires elevation"
    exit 77
}

$errorPath = [IO.Path]::GetTempFileName()
try {
    $workload = Start-Process -FilePath "cmd.exe" -ArgumentList @("/d", "/c", "ping -n 3 127.0.0.1 >nul") -WindowStyle Hidden -PassThru
    $output = & $Wperf trace --live --elevate -e sched --duration 1500 --json --max 0 2> $errorPath
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        $errorText = Get-Content -LiteralPath $errorPath -Raw -ErrorAction SilentlyContinue
        throw "wperf live trace JSON failed with exit $exitCode`n$errorText"
    }
    $json = $output -join "`n"
    $events = $json | ConvertFrom-Json
    if (@($events).Count -eq 0) {
        throw "wperf live trace JSON returned no events"
    }
    $first = @($events)[0]
    foreach ($field in @("ts", "cpu", "pid", "tid", "event", "comm")) {
        if (-not $first.PSObject.Properties.Name.Contains($field)) {
            throw "wperf live trace JSON is missing '$field'"
        }
    }
    Write-Output "live trace JSON streaming smoke: PASS"
} finally {
    if ($null -ne $workload -and -not $workload.HasExited) {
        Stop-Process -Id $workload.Id -Force -ErrorAction SilentlyContinue
    }
    Remove-Item -LiteralPath $errorPath -Force -ErrorAction SilentlyContinue
}