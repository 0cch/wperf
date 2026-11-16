[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Wperf,
    [Parameter(Mandatory = $true)][string]$FixtureDirectory,
    [Parameter(Mandatory = $true)][string]$SchemaDirectory
)

$ErrorActionPreference = "Stop"

function Assert-JsonContract {
    param(
        [string]$Name,
        [string[]]$Arguments,
        [string]$SchemaFile
    )

    $output = & $Wperf @Arguments 2>$null
    if ($LASTEXITCODE -ne 0) {
        throw "$Name command failed with exit code $LASTEXITCODE"
    }
    $json = $output -join "`n"
    if (-not (Test-Json -Json $json -SchemaFile $SchemaFile -ErrorAction Stop)) {
        throw "$Name output does not match $SchemaFile"
    }
}

$cpu = Join-Path $FixtureDirectory "cpu-stack.etl"
$sched = Join-Path $FixtureDirectory "sched.etl"
$io = Join-Path $FixtureDirectory "io.etl"
$cpuPid = (Get-Content -LiteralPath ($cpu + ".pid") -Raw).Trim()
$schedPid = (Get-Content -LiteralPath ($sched + ".pid") -Raw).Trim()
$ioPid = (Get-Content -LiteralPath ($io + ".pid") -Raw).Trim()

Assert-JsonContract "CPU report" @(
    "report", "-i", $cpu, "-p", $cpuPid, "--json", "--metadata", "--percent-limit", "0", "--quiet"
) (Join-Path $SchemaDirectory "report-cpu.schema.json")
Assert-JsonContract "streamed CPU report" @(
    "report", "-i", $cpu, "-p", $cpuPid, "--stream", "--json", "--metadata", "--percent-limit", "0", "--quiet"
) (Join-Path $SchemaDirectory "report-cpu.schema.json")

Assert-JsonContract "off-CPU report" @(
    "report", "-i", $sched, "-p", $schedPid, "--off-cpu", "--json", "--metadata",
    "--percent-limit", "0", "--quiet"
) (Join-Path $SchemaDirectory "report-off-cpu.schema.json")

Assert-JsonContract "I/O report" @(
    "report", "-i", $io, "-p", $ioPid, "--kind", "io", "--json", "--metadata", "--quiet"
) (Join-Path $SchemaDirectory "report-io.schema.json")

Assert-JsonContract "trace events" @(
    "trace", "-i", $sched, "-p", $schedPid, "-e", "sched", "--json", "--max", "10"
) (Join-Path $SchemaDirectory "trace.schema.json")

$traceOutput = & $Wperf trace -i $sched -p $schedPid -e sched --json --max 1 2>$null
if ($LASTEXITCODE -ne 0) { throw "trace field command failed" }
$traceEvent = (($traceOutput -join "`n") | ConvertFrom-Json)[0]
foreach ($field in @("event_id", "operation_id", "file_key", "file_object",
                     "stack", "stack_quality", "stack_missing",
                     "stack_ambiguous", "pmc_counters")) {
    if (-not $traceEvent.PSObject.Properties.Name.Contains($field)) {
        throw "trace JSON is missing '$field'"
    }
}

$cpuOutput = & $Wperf report -i $cpu -p $cpuPid --json --metadata --percent-limit 0 --quiet 2>$null
if ($LASTEXITCODE -ne 0) { throw "quality metadata command failed" }
$cpuJson = ($cpuOutput -join "`n") | ConvertFrom-Json
foreach ($field in @("events_total", "stack_events_total", "stack_key_mismatch",
                     "io_init_events", "pmc_counter_events", "lbr_events",
                     "lbr_branches", "samples_dropped", "unknown_events")) {
    if ($null -eq $cpuJson.quality.$field) {
        throw "quality JSON is missing '$field'"
    }
}

$symbolPath = Split-Path -Parent $Wperf
Assert-JsonContract "stat output" @(
    "stat", "--duration", "1", "--json", "--", "cmd.exe", "/d", "/c", "exit 0"
) (Join-Path $SchemaDirectory "stat.schema.json")
Assert-JsonContract "stat thread output" @(
    "stat", "-p", "$PID", "--duration", "10", "--per-thread", "--json"
) (Join-Path $SchemaDirectory "stat.schema.json")

Assert-JsonContract "trace metadata" @(
    "trace", "-i", $sched, "-p", $schedPid, "-e", "sched", "--json", "--metadata", "--max", "10"
) (Join-Path $SchemaDirectory "trace-metadata.schema.json")

Assert-JsonContract "scheduler output" @(
    "sched", "-i", $sched, "--json"
) (Join-Path $SchemaDirectory "sched.schema.json")

Assert-JsonContract "annotate output" @(
    "annotate", "-i", $cpu, "-p", $cpuPid, "--symbol-path", $symbolPath, "--disasm", "--json", "--limit", "3"
) (Join-Path $SchemaDirectory "annotate.schema.json")

Assert-JsonContract "diff output" @(
    "diff", "-1", $cpu, "-2", $cpu, "--first-pid", $cpuPid, "--second-pid", $cpuPid, "--json"
) (Join-Path $SchemaDirectory "diff.schema.json")

Assert-JsonContract "list output" @(
    "list", "--json"
) (Join-Path $SchemaDirectory "list.schema.json")

Assert-JsonContract "doctor output" @(
    "doctor", "--json"
) (Join-Path $SchemaDirectory "doctor.schema.json")

Assert-JsonContract "buildid output" @(
    "buildid-list", "-i", $cpu, "--json"
) (Join-Path $SchemaDirectory "buildid-list.schema.json")

Write-Output "json contracts: PASS"
