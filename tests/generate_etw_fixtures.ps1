[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Wperf,
    [Parameter(Mandatory = $true)][string]$Workload,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [switch]$Elevated,
    [switch]$ElevateIfNeeded
)

$ErrorActionPreference = 'Stop'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
$isAdmin = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
$requireLive = -not [string]::IsNullOrWhiteSpace($env:WPERF_REQUIRE_LIVE_TESTS) -and
               $env:WPERF_REQUIRE_LIVE_TESTS -notin @('0', 'false', 'False')

if (-not $isAdmin) {
    if (-not $requireLive -and -not $ElevateIfNeeded) {
        Write-Output 'SKIP: runtime ETW fixture generation requires elevation'
        exit 77
    }
    $quoted = @(
        '-NoProfile',
        '-ExecutionPolicy Bypass',
        '-File', ('"' + $PSCommandPath + '"'),
        '-Elevated',
        '-Wperf', ('"' + $Wperf + '"'),
        '-Workload', ('"' + $Workload + '"'),
        '-OutputDirectory', ('"' + $OutputDirectory + '"')
    ) -join ' '
    $process = Start-Process -FilePath 'pwsh.exe' -Verb RunAs -WindowStyle Hidden -Wait -PassThru -ArgumentList $quoted
    exit $process.ExitCode
}

$profiles = @(
    [pscustomobject]@{ Profile = 'cpu'; File = 'cpu-stack.etl'; Mode = 'cpu'; Frequency = 199 },
    [pscustomobject]@{ Profile = 'sched'; File = 'sched.etl'; Mode = 'sched'; NoStacks = $true },
    [pscustomobject]@{ Profile = 'syscall'; File = 'syscall.etl'; Mode = 'syscall'; NoStacks = $true },
    [pscustomobject]@{ Profile = 'io'; File = 'io.etl'; Mode = 'io'; NoStacks = $true }
)

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
foreach ($item in $profiles) {
    Remove-Item -LiteralPath (Join-Path $OutputDirectory $item.File) -Force -ErrorAction SilentlyContinue
}

$env:WPERF_FIXTURE_IO_DIR = $OutputDirectory
$generated = @()
foreach ($item in $profiles) {
    $session = 'wperf-fixture-' + $item.Profile + '-' + [guid]::NewGuid().ToString('N')
    $output = Join-Path $OutputDirectory $item.File
    $arguments = @('record') + @(
        '--profile', $item.Profile,
        '--session', $session,
        '--duration', '5000',
        '-o', $output,
        '--', $Workload, $item.Mode, $OutputDirectory
    )
    if ($item.Frequency) {
        $profileIndex = [Array]::IndexOf($arguments, '--profile')
        $insertAt = $profileIndex + 2
        $arguments = $arguments[0..($insertAt - 1)] + @('-F', $item.Frequency) +
                     $arguments[$insertAt..($arguments.Count - 1)]
    }
    if ($item.NoStacks) {
        $profileIndex = [Array]::IndexOf($arguments, '--profile')
        $insertAt = $profileIndex + 2
        $arguments = $arguments[0..($insertAt - 1)] + @('--no-call-graph') +
                     $arguments[$insertAt..($arguments.Count - 1)]
    }

    try {
        $capture = & $Wperf @arguments 2>&1 | Out-String
        if ($LASTEXITCODE -ne 0) {
            throw "failed to generate $($item.File) (exit $LASTEXITCODE):`n$capture"
        }
        if (-not (Test-Path -LiteralPath $output -PathType Leaf)) {
            throw "capture did not create $output"
        }
        $length = (Get-Item -LiteralPath $output).Length
        if ($length -lt 4096) {
            throw "capture $($item.File) is unexpectedly small ($length bytes)"
        }
        if ($capture -notmatch 'pid=(\d+)') {
            throw "capture for $($item.File) did not report the workload PID"
        }
        $fixturePid = [uint32]$Matches[1]
        Set-Content -LiteralPath ($output + '.pid') -Value $fixturePid -Encoding ascii
        $generated += [pscustomobject]@{
            profile = $item.Profile
            file = $item.File
            bytes = $length
            pid = $fixturePid
        }
        Write-Output "generated runtime ETW fixture: $($item.File) ($length bytes)"
    }
    finally {
        logman stop $session -ets 2>$null | Out-Null
    }
}

$generated | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'manifest.json') -Encoding utf8NoBOM
exit 0
