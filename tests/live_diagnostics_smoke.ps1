[CmdletBinding()]
param([Parameter(Mandatory)][string]$Wperf, [string]$OutputDirectory = '',
      [string[]]$Profiles = @('dpc','isr','heap','pool','driver'))
$ErrorActionPreference = 'Stop'
# Keep this live test hermetic: report symbolization uses the local build
# directory and must not block on a machine-level symbol server.
$previousSymbolPath = $env:_NT_SYMBOL_PATH
$env:_NT_SYMBOL_PATH = ''
$principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Output 'SKIP: live diagnostic capture requires elevation'
    exit 77
}
$keep = -not [string]::IsNullOrEmpty($OutputDirectory)
$directory = if ($keep) { $OutputDirectory } else { Join-Path ([IO.Path]::GetTempPath()) ('wperf-live-diagnostics-' + [guid]::NewGuid().ToString('N')) }
New-Item -ItemType Directory -Path $directory -Force | Out-Null
$bin = Split-Path -Parent $Wperf
$workload = Join-Path $bin 'wperf_diagnostic_workload.exe'
$results = @()
function Start-Hidden {
    param([string]$Exe, [string[]]$Arguments, [string]$Prefix)
    $quoted = ($Arguments | ForEach-Object { '"' + $_.Replace('"','\"') + '"' }) -join ' '
    $process = Start-Process -FilePath $Exe -ArgumentList $quoted -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput "$Prefix.stdout" -RedirectStandardError "$Prefix.stderr"
    [void]$process.Handle
    return $process
}
try {
    foreach ($profile in $Profiles) {
        $prefix = Join-Path $directory $profile
        $go = "$prefix.go"; $stop = "$prefix.stop"; $etl = "$prefix.etl"
        $mode = if ($profile -eq 'heap') { 'heap' } else { 'io' }
        $target = Start-Hidden $workload @($mode,$go,$stop,"$prefix.data") "$prefix.target"
        $capture = $null
        try {
            $arguments = @('record','--profile',$profile,'--duration','1800','-o',$etl)
            if ($profile -eq 'heap') { $arguments += @('--pid', [string]$target.Id) }
            if ($profile -eq 'pool') { $arguments += @('--pool-tag','File') }
            $capture = Start-Hidden $Wperf $arguments "$prefix.capture"
            $deadline = [DateTime]::UtcNow.AddSeconds(15)
            do {
                if ($capture.HasExited) { throw "$profile capture exited before READY: $(Get-Content "$prefix.capture.stderr" -Raw)" }
                $ready = (Test-Path "$prefix.capture.stdout") -and ((Get-Content "$prefix.capture.stdout" -Raw) -match 'capturing:')
                if (-not $ready) { Start-Sleep -Milliseconds 50 }
            } while (-not $ready -and [DateTime]::UtcNow -lt $deadline)
            if (-not $ready) { throw "$profile capture never reached READY" }
            New-Item -ItemType File -Path $go | Out-Null
            if (-not $capture.WaitForExit(20000)) { throw "$profile capture did not finish" }
            if ($capture.ExitCode -ne 0) { throw "$profile capture failed ($($capture.ExitCode)): $(Get-Content "$prefix.capture.stderr" -Raw)" }
            $kinds = if ($profile -eq 'kernel') { @('dpc','isr') } else { @($profile) }
            foreach ($kind in $kinds) {
                $jsonFile = "$prefix.$kind.json"
                & $Wperf report -i $etl --kind $kind --json --metadata --symbol-path $bin --quiet -o $jsonFile
                if ($LASTEXITCODE -ne 0) { throw "$kind report failed: $LASTEXITCODE" }
                $doc = Get-Content -LiteralPath $jsonFile -Raw | ConvertFrom-Json
                if (@($doc.rows).Count -eq 0) { throw "$kind report is empty" }
                if ($doc.quality.decode_errors -ne 0 -or $doc.quality.diagnostic_unsupported -ne 0 -or $doc.quality.lost_events -ne 0) {
                    throw "$kind capture quality: decode=$($doc.quality.decode_errors), unsupported=$($doc.quality.diagnostic_unsupported), lost=$($doc.quality.lost_events)"
                }
                if ($kind -in @('dpc','isr','pool') -and @($doc.rows | Where-Object { $null -ne $_.pid -or $_.owner -ne 'system' }).Count) { throw "$kind invented process ownership" }
                if ($kind -eq 'heap') {
                    $heap = [uint64](Get-Content "$prefix.target.stdout" | Select-Object -First 1)
                    $rows = @($doc.rows | Where-Object heap -eq $heap)
                    if ($rows.Count -eq 0 -or ($rows | Measure-Object live_bytes -Sum).Sum -lt 65536 -or
                        ($rows | Measure-Object freed_bytes -Sum).Sum -le 0) { throw 'Heap held allocations/realloc/free were not reconstructed' }
                    if (@($rows | Where-Object { $_.allocation_stack.Count -gt 0 }).Count -eq 0) { throw 'Heap allocation stacks were not captured' }
                    if ($doc.quality.analysis.address_reuse -ne 0) { throw 'Heap lifecycle contains unexplained address reuse' }
                }
                if ($kind -eq 'driver' -and @($doc.rows | Where-Object { $_.semantics -eq 'observed-call-span' -and $_.max_ns -gt 0 }).Count -eq 0) { throw 'No bounded driver call was measured' }
                if ($kind -eq 'driver' -and @($doc.rows | Where-Object { $_.pid -eq $target.Id -and $_.context_basis -eq 'scheduler' }).Count -eq 0) { throw 'Driver request context did not map to the I/O workload' }
                if ($kind -eq 'pool' -and @($doc.rows | Where-Object tag -ne 'File').Count) { throw 'Native pool tag capture filter was not honored' }
                $results += [pscustomobject]@{Kind=$kind;Rows=@($doc.rows).Count;Quality=$doc.quality}
                Write-Output "live $kind capture/replay/report: PASS ($(@($doc.rows).Count) rows)"
            }
        } finally {
            New-Item -ItemType File -Path $stop -Force | Out-Null
            if ($capture -and -not $capture.HasExited) { $capture.Kill(); $capture.WaitForExit() }
            if (-not $target.WaitForExit(5000)) { $target.Kill(); $target.WaitForExit() }
        }
    }
    $results | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $directory 'results.json') -Encoding utf8
} finally {
    if (-not $keep) {
        foreach ($file in Get-ChildItem -LiteralPath $directory -File) { Remove-Item -LiteralPath $file.FullName -Force }
        Remove-Item -LiteralPath $directory
    }
    $env:_NT_SYMBOL_PATH = $previousSymbolPath
}
