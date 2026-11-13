param(
    [Parameter(Mandatory=$true)][string]$Wperf,
    [Parameter(Mandatory=$true)][string]$Fixture,
    [Parameter(Mandatory=$true)][string]$SymbolPath
)
$ErrorActionPreference = 'Stop'
$report = & $Wperf report -i $Fixture --json --metadata --quiet --symbol-path $SymbolPath |
    Out-String | ConvertFrom-Json
if ($LASTEXITCODE -ne 0) { throw 'JSON replay failed' }
if ($report.quality.samples -ne 17323 -or $report.quality.exact_stacks -ne 16104) {
    throw 'Replay metadata does not match the CPU fixture'
}
if ($report.quality.approximate_stacks -ne 0 -or $report.quality.missing_stacks -ne 1219) {
    throw 'Unsafe approximate stacks were reattached'
}
if ($report.quality.pmc_samples -ne 0 -or $report.quality.incomplete) {
    throw 'CPU fixture is incorrectly classified'
}
$total = ($report.rows | Measure-Object -Property samples -Sum).Sum
if ($total -ne $report.quality.samples) { throw 'JSON rows lose CPU samples' }
$difference = & $Wperf diff -1 $Fixture -2 $Fixture --json --symbol-path $SymbolPath |
    Out-String | ConvertFrom-Json
if ($LASTEXITCODE -ne 0 -or $difference.Count -ne 0) { throw 'Identical traces must have an empty diff' }
$script = & $Wperf script -i $Fixture --limit 1 -F source,period,stack_quality --csv --symbol-path $SymbolPath |
    ConvertFrom-Csv
if ($LASTEXITCODE -ne 0 -or $script.source -ne '0' -or $script.stack_quality -notin @('exact','missing')) {
    throw 'Script does not preserve sample provenance'
}
Write-Output 'replay contract: PASS'
