[CmdletBinding()]
param(
    [string]$OutputDir = "",
    [int]$TraceSeconds = 3,
    [switch]$SkipXperf
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $OutputDir = Join-Path $repo "artifacts\m0"
}
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

$cmake = "C:\Program Files\CMake\bin\cmake.exe"
$wperf = Join-Path $repo "build\Release\wperf.exe"
$workload = Join-Path $repo "build\Release\wperf_cpu_burn.exe"

if (-not (Test-Path $wperf)) {
    & $cmake -S $repo -B (Join-Path $repo "build") -G "Visual Studio 17 2022" -A x64
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
    & $cmake --build (Join-Path $repo "build") --config Release --parallel
    if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }
}

function Invoke-Capture {
    param(
        [string]$FilePath,
        [string[]]$Arguments,
        [string]$OutputFile,
        [switch]$AllowFailure
    )
    $output = & $FilePath @Arguments 2>&1
    $exitCode = $LASTEXITCODE
    $output | Out-File -LiteralPath $OutputFile -Encoding utf8
    if (-not $AllowFailure -and $exitCode -ne 0) {
        throw "$FilePath exited with $exitCode; see $OutputFile"
    }
    return $exitCode
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
$elevated = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

$environment = [ordered]@{
    timestamp = (Get-Date).ToString("o")
    user = $identity.Name
    elevated = $elevated
    computer = $env:COMPUTERNAME
    os = (Get-CimInstance Win32_OperatingSystem | Select-Object Caption, Version, BuildNumber, OSArchitecture)
    cpu = (Get-CimInstance Win32_Processor | Select-Object Name, Manufacturer, NumberOfCores, NumberOfLogicalProcessors)
    dotnet = (& dotnet --version 2>$null)
    cmake = (& $cmake --version | Select-Object -First 1)
}
$environment | ConvertTo-Json -Depth 6 | Out-File -LiteralPath (Join-Path $OutputDir "environment.json") -Encoding utf8

Invoke-Capture -FilePath $wperf -Arguments @("--version") -OutputFile (Join-Path $OutputDir "wperf-version.txt") | Out-Null
Invoke-Capture -FilePath $wperf -Arguments @("list") -OutputFile (Join-Path $OutputDir "wperf-list.txt") | Out-Null
Invoke-Capture -FilePath $wperf -Arguments @("doctor") -OutputFile (Join-Path $OutputDir "wperf-doctor.txt") | Out-Null

$xperf = $null
$knownXperf = "C:\Program Files (x86)\Windows Kits\10\Windows Performance Toolkit\xperf.exe"
if (Test-Path $knownXperf) {
    $xperf = $knownXperf
} else {
    $command = Get-Command xperf.exe -ErrorAction SilentlyContinue
    if ($command) { $xperf = $command.Source }
}

$summary = [ordered]@{
    timestamp = (Get-Date).ToString("o")
    elevated = $elevated
    wperf = $wperf
    xperf = $xperf
    xperfTrace = "skipped"
    xperfTraceReason = ""
    lbrHelpPresent = $false
}

if (-not (Test-Path $workload)) {
    & $cmake --build (Join-Path $repo "build") --config Release --parallel
    if ($LASTEXITCODE -ne 0) { throw "workload build failed" }
}

if (-not $SkipXperf -and $xperf) {
    $helpFile = Join-Path $OutputDir "xperf-help-start.txt"
    Invoke-Capture -FilePath $xperf -Arguments @("-help", "start") -OutputFile $helpFile -AllowFailure | Out-Null
    $summary.lbrHelpPresent = (Get-Content -LiteralPath $helpFile -Raw) -match "-LastBranch"

    if (-not $elevated) {
        $summary.xperfTraceReason = "xperf capture requires an elevated process"
    } else {
        $etl = Join-Path $OutputDir "m0-cpu.etl"
        Remove-Item -LiteralPath $etl -Force -ErrorAction SilentlyContinue
        $startArgs = @(
            "-on", "PROC_THREAD+LOADER+PROFILE",
            "-stackwalk", "Profile",
            "-SetProfInt", "10000",
            "-BufferSize", "256",
            "-MaxBuffers", "64",
            "-FileMode", "Circular",
            "-MaxFile", "1024",
            "-f", $etl
        )
        $started = $false
        try {
            Invoke-Capture -FilePath $xperf -Arguments $startArgs -OutputFile (Join-Path $OutputDir "xperf-start.txt") | Out-Null
            $started = $true
            Invoke-Capture -FilePath $workload -Arguments @(($TraceSeconds * 1000).ToString(), "4") -OutputFile (Join-Path $OutputDir "cpu-burn.txt") | Out-Null
            Invoke-Capture -FilePath $xperf -Arguments @("-d", $etl) -OutputFile (Join-Path $OutputDir "xperf-stop.txt") | Out-Null
            $started = $false
            Invoke-Capture -FilePath $xperf -Arguments @("-i", $etl, "-symbols", "-a", "profile", "-detail") -OutputFile (Join-Path $OutputDir "xperf-profile.txt") | Out-Null
            $summary.xperfTrace = "complete"
        } catch {
            $summary.xperfTrace = "failed"
            $summary.xperfTraceReason = $_.Exception.Message
        } finally {
            if ($started) {
                & $xperf -stop 2>&1 | Out-File -LiteralPath (Join-Path $OutputDir "xperf-cleanup.txt") -Encoding utf8
            }
        }
    }
} elseif ($SkipXperf) {
    $summary.xperfTraceReason = "disabled by -SkipXperf"
} else {
    $summary.xperfTraceReason = "xperf not found"
}

$summary | ConvertTo-Json -Depth 5 | Out-File -LiteralPath (Join-Path $OutputDir "summary.json") -Encoding utf8
Write-Host "M0 probe complete: $OutputDir"
Write-Host ("  elevated: {0}" -f $elevated)
Write-Host ("  xperf: {0}" -f ($(if ($xperf) { $xperf } else { "not found" })))
Write-Host ("  xperf trace: {0}" -f $summary.xperfTrace)
if ($summary.xperfTraceReason) {
    Write-Host ("  reason: {0}" -f $summary.xperfTraceReason)
}
