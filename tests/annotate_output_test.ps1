[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Wperf,[Parameter(Mandatory=$true)][string]$Fixture,[Parameter(Mandatory=$true)][string]$SymbolPath)
$ErrorActionPreference='Stop'
$text = & $Wperf annotate -i $Fixture --symbol-path $SymbolPath --disasm --json --limit 5 2>$null
if ($LASTEXITCODE -ne 0) { throw "annotate --disasm --json failed: $LASTEXITCODE" }
$rows = (($text -join "`n") | ConvertFrom-Json)
if (@($rows).Count -eq 0) { throw 'annotate returned no rows' }
foreach ($field in @('samples','module','symbol','source_text','function_rva','instruction_rows')) {
    if (-not $rows[0].PSObject.Properties.Name.Contains($field)) { throw "annotate JSON missing $field" }
}
$instructionRow = @($rows | Where-Object { @($_.instruction_rows).Count -gt 0 }) | Select-Object -First 1
if ($null -ne $instructionRow) {
    $instruction = @($instructionRow.instruction_rows)[0]
    foreach ($field in @('instruction_rva','bytes','text','samples','overhead')) {
        if (-not $instruction.PSObject.Properties.Name.Contains($field)) { throw "annotate instruction missing $field" }
    }
}
