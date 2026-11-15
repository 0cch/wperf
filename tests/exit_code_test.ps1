[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Wperf)

$ErrorActionPreference = "Stop"
$failures = [System.Collections.Generic.List[string]]::new()

function Invoke-Case {
    param([string]$Name, [int]$Expected, [string[]]$Arguments)
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $output = & $Wperf @Arguments 2>&1
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $old
    }
    if ($code -ne $Expected) {
        $failures.Add("$Name expected $Expected, got $code`: $($output -join ' ')")
    }
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
$recordWithoutElevateExit = if ($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { 0 } else { 3 }
Invoke-Case "record privilege behavior" $recordWithoutElevateExit @("record", "--duration", "1")
Invoke-Case "target launch failure" 7 @("stat", "--duration", "1", "--", "wperf-command-that-does-not-exist-7d43e0")
Invoke-Case "target nonzero exit" 8 @("stat", "--duration", "1000", "--", "cmd.exe", "/d", "/c", "exit 42")

if ($failures.Count -ne 0) {
    $failures | ForEach-Object { Write-Error $_ }
    exit 1
}
Write-Output "exit codes: PASS"
