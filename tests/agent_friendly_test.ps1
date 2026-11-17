[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Wperf,
    [Parameter(Mandatory = $true)][string]$SchemaDirectory
)

$ErrorActionPreference = "Stop"

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Invoke-Process {
    param([string[]]$Arguments)
    $start = [System.Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $Wperf
    $start.UseShellExecute = $false
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    foreach ($argument in $Arguments) {
        $start.ArgumentList.Add($argument)
    }
    $process = [System.Diagnostics.Process]::Start($start)
    $stdout = $process.StandardOutput.ReadToEnd()
    $stderr = $process.StandardError.ReadToEnd()
    $process.WaitForExit()
    return [pscustomobject]@{
        ExitCode = $process.ExitCode
        Stdout = $stdout
        Stderr = $stderr
    }
}

$capabilityResult = Invoke-Process @("capabilities", "--json")
Assert-True ($capabilityResult.ExitCode -eq 0) "capabilities failed with $($capabilityResult.ExitCode): $($capabilityResult.Stderr)"
$capabilitySchema = Join-Path $SchemaDirectory "capabilities.schema.json"
Assert-True (Test-Json -Json $capabilityResult.Stdout -SchemaFile $capabilitySchema -ErrorAction Stop) "capabilities does not match capabilities.schema.json"
$capabilities = $capabilityResult.Stdout | ConvertFrom-Json
$record = $capabilities.commands | Where-Object { $_.name -eq "record" }
Assert-True ($null -ne $record) "capabilities is missing record"
Assert-True ($record.requires_elevation -eq $true) "record should report requires_elevation"
Assert-True ($record.supports_json -eq $true) "record should report supports_json"
Assert-True (($record.options | Where-Object { $_.names -contains "--json" }).Count -eq 1) "record should expose --json"
Assert-True (($capabilities.global_options | Where-Object { $_.names -contains "--json-errors" }).Count -eq 1) "capabilities is missing --json-errors"

$errorResult = Invoke-Process @("--json-errors", "report")
Assert-True ($errorResult.ExitCode -eq 2) "structured report error should exit 2, got $($errorResult.ExitCode)"
$errorSchema = Join-Path $SchemaDirectory "agent-error.schema.json"
Assert-True (Test-Json -Json $errorResult.Stderr -SchemaFile $errorSchema -ErrorAction Stop) "structured error does not match agent-error.schema.json"
Assert-True (($errorResult.Stderr.Trim() -split "`r?`n").Count -eq 1) "structured error should be one line"
$errorJson = $errorResult.Stderr | ConvertFrom-Json
Assert-True ($errorJson.ok -eq $false) "structured error should set ok=false"
Assert-True ($errorJson.command -eq "report") "structured error should identify the command"
Assert-True ($errorJson.error.code -eq "invalid_arguments") "structured error should map exit 2 to invalid_arguments"
Assert-True ($errorJson.error.action -eq "fix_arguments") "structured error should provide a remediation action"
Assert-True (-not [string]::IsNullOrWhiteSpace($errorJson.error.message)) "structured error should include a diagnostic message"

$postCommandFlag = Invoke-Process @("report", "--json-errors")
Assert-True ($postCommandFlag.ExitCode -eq 2) "post-command --json-errors should preserve exit 2"
Assert-True ($postCommandFlag.Stderr -match '"code"\s*:\s*"invalid_arguments"') "post-command --json-errors should emit structured JSON"

$helpOutput = (& $Wperf record --help 2>$null) -join "`n"
Assert-True ($LASTEXITCODE -eq 0) "record help failed"
Assert-True ($helpOutput -match '--json') "record help should document --json"

Write-Output "agent-friendly contract: PASS"