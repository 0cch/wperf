[CmdletBinding()]
param([Parameter(Mandatory)][string]$Wperf)
$ErrorActionPreference='Stop'
$principal=[Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if(-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Output 'SKIP: live kernel image validation requires elevation'
    exit 77
}
$directory=Join-Path ([IO.Path]::GetTempPath()) ('wperf-kernel-image-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $directory|Out-Null
$trace=Join-Path $directory 'kernel.etl'
$captureOutput=Join-Path $directory 'capture.stdout'
$errors=Join-Path $directory 'capture.stderr'
try {
    $burn=Join-Path (Split-Path -Parent $Wperf) 'wperf_cpu_burn.exe'
    & $Wperf record -F 199 --duration 1500 -o $trace -- $burn 1200 2 >$captureOutput 2>$errors
    if($LASTEXITCODE -ne 0){throw "Kernel capture failed: $LASTEXITCODE`n$(Get-Content -LiteralPath $errors -Raw)"}
    $output=& $Wperf buildid-list -i $trace --json
    if($LASTEXITCODE -ne 0){throw 'Kernel module replay failed'}
    $modules=@(($output -join "`n")|ConvertFrom-Json)
    $threshold=[bigint]::Parse('18446603336221196288')
    $kernel=@($modules|Where-Object {[bigint]$_.base -ge $threshold})
    if($kernel.Count -eq 0){throw 'No kernel image rundown records captured'}
    if(@($kernel|Where-Object {$_.pid -ne 0 -and $_.pid -ne 4}).Count -ne 0){throw 'Kernel rundown inherited collector PID'}
    $reportText=& $Wperf report -i $trace --scope kernel --json --metadata --quiet --symbol-path (Split-Path -Parent $Wperf)
    if($LASTEXITCODE -ne 0){throw 'Kernel CPU report replay failed'}
    $report=($reportText -join "`n")|ConvertFrom-Json
    if(@($report.rows).Count -eq 0){throw 'No kernel CPU samples captured'}
    if(@($report.rows|Where-Object dso -ieq 'ntoskrnl.exe').Count -eq 0){throw 'Kernel samples did not map to recorded ntoskrnl image'}
    if($report.quality.lost_events -ne 0 -or $report.quality.decode_errors -ne 0){throw 'Kernel capture quality was insufficient'}
    Write-Output "live kernel image ownership/module report: PASS ($($kernel.Count) kernel mappings)"
} finally {
    foreach($file in @($trace,$captureOutput,$errors)) {Remove-Item -LiteralPath $file -Force -ErrorAction SilentlyContinue}
    Remove-Item -LiteralPath $directory -ErrorAction SilentlyContinue
}
