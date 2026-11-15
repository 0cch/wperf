[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Wperf
)

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Output "SKIP: live PMC JSON smoke requires elevation"
    exit 77
}

$errorPath = [IO.Path]::GetTempFileName()
try {
    $output = & $Wperf stat --pmu -e TotalCycles --duration 750 --json -- cmd.exe /d /c "ping -n 2 127.0.0.1 >nul" 2> $errorPath
    $exitCode = $LASTEXITCODE
    if ($exitCode -eq 5) {
        Write-Output "SKIP: PMC profile source is unavailable"
        exit 77
    }
    if ($exitCode -ne 0) {
        $errorText = Get-Content -LiteralPath $errorPath -Raw -ErrorAction SilentlyContinue
        throw "wperf PMC stat JSON failed with exit $exitCode`n$errorText"
    }
    $result = (($output -join "`n") | ConvertFrom-Json)
    foreach ($field in @("source", "exact", "profile_source", "sample_count", "samples", "quality")) {
        if (-not $result.PSObject.Properties.Name.Contains($field)) {
            throw "wperf PMC stat JSON is missing '$field'"
        }
    }
    if ($result.source -ne "etw-pmc-sampling" -or $result.exact -ne $false) {
        throw "wperf PMC stat JSON has unexpected semantics"
    }
    Write-Output "live PMC JSON smoke: PASS"
} finally {
    Remove-Item -LiteralPath $errorPath -Force -ErrorAction SilentlyContinue
}