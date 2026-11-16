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
$snapshotPath = Join-Path ([IO.Path]::GetTempPath()) ('wperf-pmc-snapshot-test-' + [guid]::NewGuid().ToString('N') + '.etl')
$passed = $false
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
    $source = [string]$result.profile_source
    $capture = & $Wperf record --pmc-counters $source --no-call-graph --duration 750 -o $snapshotPath 2>$errorPath
    if ($LASTEXITCODE -ne 0) { throw "PMC snapshot capture failed: $LASTEXITCODE`n$(Get-Content -LiteralPath $errorPath -Raw)" }
    $snapshot = & $Wperf report -i $snapshotPath --kind pmc --source $source --json --metadata --quiet
    if ($LASTEXITCODE -ne 0) { throw 'PMC snapshot report failed' }
    $snapshotJson = (($snapshot -join "`n") | ConvertFrom-Json)
    if (@($snapshotJson.rows).Count -eq 0) { throw 'PMC snapshots produced no rows' }
    $csv = & $Wperf report -i $snapshotPath --kind pmc --csv --quiet
    if ($LASTEXITCODE -ne 0 -or $csv[0] -ne 'cpu,source,pid,tid,delta,intervals,exact,semantics') { throw 'PMC snapshot CSV contract failed' }
    foreach($row in ($csv | ConvertFrom-Csv)) {
        if($row.exact -ne 'false' -or $row.semantics -ne 'event-snapshot-derived') { throw 'PMC CSV lost estimated semantics' }
    }
    $empty = & $Wperf report -i $snapshotPath --kind pmc --cpu 4294967294 --json --quiet
    if ($LASTEXITCODE -ne 5 -or @((($empty -join "`n")|ConvertFrom-Json)).Count -ne 0) { throw 'PMC CPU filtering failed' }
    $emptyTime = & $Wperf report -i $snapshotPath --kind pmc --time-start 999999999 --json --quiet
    if ($LASTEXITCODE -ne 5) { throw 'PMC time filtering failed' }
    $strict = & $Wperf report -i $snapshotPath --kind pmc --json --strict-quality --quiet
    $expected = if($snapshotJson.quality.incomplete) { 6 } else { 0 }
    if ($LASTEXITCODE -ne $expected) { throw 'PMC strict quality gate was ignored' }
    Write-Output "live PMC JSON smoke: PASS"
    $passed = $true
} finally {
    if(-not $passed -and (Test-Path -LiteralPath $snapshotPath)) {
        Copy-Item -LiteralPath $snapshotPath -Destination (Join-Path (Split-Path -Parent $Wperf) 'priority-pmc-failed.etl') -Force
    }
    Remove-Item -LiteralPath $errorPath -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $snapshotPath -Force -ErrorAction SilentlyContinue
}
