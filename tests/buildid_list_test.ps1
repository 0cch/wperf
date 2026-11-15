[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Wperf,[Parameter(Mandatory=$true)][string]$Fixture)
$ErrorActionPreference='Stop'
$jsonText = & $Wperf buildid-list -i $Fixture --json 2>$null
if ($LASTEXITCODE -ne 0) { throw "buildid-list --json failed: $LASTEXITCODE" }
$rows = (($jsonText -join "`n") | ConvertFrom-Json)
if (@($rows).Count -eq 0) { throw 'buildid-list returned no modules' }
foreach ($field in @('base','size','pid','pe_timestamp','pe_checksum','machine','pdb_guid','pdb_age','path','module_sha256','status')) {
    if (-not $rows[0].PSObject.Properties.Name.Contains($field)) { throw "buildid-list JSON missing $field" }
}
$csvText = & $Wperf buildid-list -i $Fixture --csv 2>$null
if ($LASTEXITCODE -ne 0) { throw "buildid-list --csv failed: $LASTEXITCODE" }
if ($csvText[0] -ne 'base,size,pid,pe_timestamp,pe_checksum,machine,pdb_guid,pdb_age,module_sha256,path,status') { throw "unexpected CSV header: $($csvText[0])" }
Write-Output 'buildid-list: PASS'
