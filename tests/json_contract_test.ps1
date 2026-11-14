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

Assert-JsonContract "CPU report" @(
    "report", "-i", $cpu, "--json", "--metadata", "--percent-limit", "0", "--quiet"
) (Join-Path $SchemaDirectory "report-cpu.schema.json")

Assert-JsonContract "off-CPU report" @(
    "report", "-i", $sched, "--off-cpu", "--json", "--metadata",
    "--percent-limit", "0", "--quiet"
) (Join-Path $SchemaDirectory "report-off-cpu.schema.json")

Assert-JsonContract "I/O report" @(
    "report", "-i", $io, "--kind", "io", "--json", "--metadata", "--quiet"
) (Join-Path $SchemaDirectory "report-io.schema.json")

Assert-JsonContract "trace events" @(
    "trace", "-i", $sched, "-e", "sched", "--json", "--max", "10"
) (Join-Path $SchemaDirectory "trace.schema.json")

Write-Output "json contracts: PASS"