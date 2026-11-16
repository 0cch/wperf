[CmdletBinding()]
param([Parameter(Mandatory)][string]$Tests, [Parameter(Mandatory)][string]$Wperf,
      [Parameter(Mandatory)][string]$SchemaDirectory, [Parameter(Mandatory)][string]$Fixture)
$ErrorActionPreference = 'Stop'
$directory = Join-Path ([IO.Path]::GetTempPath()) ('wperf-diagnostic-contract-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $directory | Out-Null
try {
    & $Tests --contracts $directory
    if ($LASTEXITCODE -ne 0) { throw 'Diagnostic decoder/analysis tests failed' }
    foreach ($kind in @('dpc','isr','interrupt','driver','heap','pool')) {
        $json = Get-Content -LiteralPath (Join-Path $directory "$kind.json") -Raw
        $schema = if ($kind -in @('heap','pool')) { 'diagnostic-memory.schema.json' } else { 'diagnostic-execution.schema.json' }
        if (-not (Test-Json -Json $json -SchemaFile (Join-Path $SchemaDirectory $schema))) { throw "Invalid $kind JSON" }
        $doc = $json | ConvertFrom-Json
        if (@($doc.rows).Count -eq 0) { throw "Empty $kind contract fixture" }
        & $Wperf report -i $Fixture --kind $kind --json --metadata --quiet > (Join-Path $directory 'empty.json') 2>$null
        if ($LASTEXITCODE -ne 5) { throw "$kind empty selection should exit 5, got $LASTEXITCODE" }
        if (-not (Test-Json -Json (Get-Content (Join-Path $directory 'empty.json') -Raw) -SchemaFile (Join-Path $SchemaDirectory $schema))) { throw "Invalid empty $kind envelope" }
    }
    $invalid = @(
        @('report','--kind','dpc','--pid','42'), @('report','--kind','pool','--tid','43'),
        @('report','--kind','interrupt','--comm','sample'), @('report','--kind','driver','--stream'),
        @('report','--kind','heap','--folded'), @('report','--kind','heap','--weight','samples'),
        @('report','--kind','driver','--max-missing-stacks','0'), @('report','--kind','heap','--source','0'),
        @('record','--profile','heap'), @('record','--profile','heap','--pid','0'),
        @('record','--profile','pool','-F','99'), @('record','--profile','driver','--lbr'),
        @('record','--profile','pool'), @('record','--profile','pool','--pool-tag','bad'),
        @('record','--pool-tag','File'), @('list','providers','--pool-tags')
    )
    foreach ($arguments in $invalid) {
        & $Wperf @arguments 2>$null | Out-Null
        if ($LASTEXITCODE -ne 2) { throw "Expected invalid arguments: $($arguments -join ' '), got $LASTEXITCODE" }
    }
    $tags = & $Wperf list --pool-tags --json --max 3
    if ($LASTEXITCODE -ne 0) { throw 'System pool tag snapshot query failed' }
    $tagJson = $tags -join "`n"
    if (-not (Test-Json -Json $tagJson -SchemaFile (Join-Path $SchemaDirectory 'list.schema.json'))) { throw 'Invalid pool tag list schema' }
    $tagDoc = $tagJson | ConvertFrom-Json
    if (@($tagDoc.pool_tags).Count -gt 3 -or $tagDoc.semantics -ne 'system-pool-tag-snapshot') { throw 'Pool tag limit/semantics mismatch' }
    Write-Output 'diagnostic schemas, empty-output contracts, and incompatible options: PASS'
} finally {
    foreach ($file in Get-ChildItem -LiteralPath $directory -File) { Remove-Item -LiteralPath $file.FullName -Force }
    Remove-Item -LiteralPath $directory
}
