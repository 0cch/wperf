param(
    [Parameter(Mandatory=$true)][string]$Wperf,
    [Parameter(Mandatory=$true)][string]$Fixture,
    [Parameter(Mandatory=$true)][string]$SymbolPath
)
$ErrorActionPreference = 'Stop'
$fixturePid = (Get-Content -LiteralPath ($Fixture + ".pid") -Raw).Trim()
$report = & $Wperf report -i $Fixture -p $fixturePid --json --metadata --quiet --symbol-path $SymbolPath |
    Out-String | ConvertFrom-Json
if ($LASTEXITCODE -ne 0) { throw 'JSON replay failed' }
if ($report.quality.samples -le 0 -or $report.quality.exact_stacks -le 0) {
    throw 'Runtime CPU replay did not produce samples and exact stacks'
}
if ($report.quality.approximate_stacks -gt $report.quality.exact_stacks) {
    throw 'Runtime replay reattached unsafe approximate stacks'
}
if ($report.quality.pmc_samples -ne 0 -or $report.quality.incomplete) {
    throw 'Runtime CPU capture is incorrectly classified'
}
$total = ($report.rows | Measure-Object -Property samples -Sum).Sum
if ($total -le 0 -or $total -gt $report.quality.samples) { throw 'JSON rows lose CPU samples' }
$difference = & $Wperf diff -1 $Fixture -2 $Fixture --first-pid $fixturePid --second-pid $fixturePid --json --symbol-path $SymbolPath |
    Out-String | ConvertFrom-Json
if ($LASTEXITCODE -ne 0 -or $difference.Count -ne 0) { throw 'Identical traces must have an empty diff' }
$script = & $Wperf script -i $Fixture -p $fixturePid --limit 1 -F source,period,stack_quality --csv --symbol-path $SymbolPath |
    ConvertFrom-Csv
if ($LASTEXITCODE -ne 0 -or $script.source -ne '0' -or $script.stack_quality -notin @('exact','missing')) {
    throw 'Script does not preserve sample provenance'
}
Write-Output 'replay contract: PASS'
