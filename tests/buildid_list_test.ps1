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
foreach($row in $rows) {
    if($row.pe_timestamp -ne $row.recorded_identity.pe_timestamp -or $row.pe_checksum -ne $row.recorded_identity.pe_checksum) { throw 'Recorded identity was replaced by local file identity' }
    if($row.status -notin @('matching','mismatched','missing') -or $row.hash_status -ne 'not-requested' -or $row.module_sha256 -ne '') { throw 'Invalid module status or implicit hashing' }
    if($row.local_identity.valid -and $row.recorded_identity.pe_timestamp -ne 0 -and $row.recorded_identity.pe_timestamp -ne $row.local_identity.pe_timestamp -and $row.status -ne 'mismatched') { throw 'Mismatched local module was accepted' }
}
$csvText = & $Wperf buildid-list -i $Fixture --csv 2>$null
if ($LASTEXITCODE -ne 0) { throw "buildid-list --csv failed: $LASTEXITCODE" }
if ($csvText[0] -ne 'base,size,pid,pe_timestamp,pe_checksum,machine,pdb_guid,pdb_age,module_sha256,path,status,local_pe_timestamp,local_pe_checksum,local_size_of_image,hash_status') { throw "unexpected CSV header: $($csvText[0])" }
Write-Output 'buildid-list: PASS'
