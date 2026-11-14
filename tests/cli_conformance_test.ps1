[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Wperf,
    [Parameter(Mandatory = $true)][string]$ReferencePath
)

$ErrorActionPreference = "Stop"
$failures = [System.Collections.Generic.List[string]]::new()

function Normalize-Text([string]$Text) {
    return (($Text -replace "`r`n", "`n").TrimEnd())
}

function Invoke-Wperf {
    param([string[]]$Arguments)
    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $output = & $Wperf @Arguments 2>&1
        return [pscustomobject]@{
            ExitCode = $LASTEXITCODE
            Output = ($output | ForEach-Object { $_.ToString() }) -join "`n"
        }
    } finally {
        $ErrorActionPreference = $oldPreference
    }
}

function Expect-Success {
    param([string]$Name, [string[]]$Arguments)
    $result = Invoke-Wperf $Arguments
    if ($result.ExitCode -ne 0) {
        $failures.Add("$Name exited $($result.ExitCode): $($result.Output)")
    }
    return $result
}

function Expect-Exit {
    param([string]$Name, [int]$Code, [string[]]$Arguments)
    $result = Invoke-Wperf $Arguments
    if ($result.ExitCode -ne $Code) {
        $failures.Add("$Name expected exit $Code, got $($result.ExitCode)")
    }
}

$commands = @(
    "record", "report", "script", "stat", "top", "trace", "annotate",
    "diff", "sched", "archive", "buildid-list", "list", "doctor",
    "test", "recover", "help", "version"
)

$general = Expect-Success "general help" @("--help")
foreach ($command in $commands) {
    if ($general.Output -notmatch [regex]::Escape($command)) {
        $failures.Add("general help does not list '$command'")
    }
}

foreach ($command in $commands) {
    if ($command -in @("help", "version")) { continue }
    $short = Expect-Success "help $command" @("help", $command)
    $direct = Expect-Success "$command --help" @($command, "--help")
    if ((Normalize-Text $short.Output) -ne (Normalize-Text $direct.Output)) {
        $failures.Add("'help $command' and '$command --help' differ")
    }
}
$targeted = Expect-Success "targeted global help" @("--help", "report")
$reportHelp = Expect-Success "report help" @("help", "report")
if ((Normalize-Text $targeted.Output) -ne (Normalize-Text $reportHelp.Output)) {
    $failures.Add("'--help report' and 'help report' differ")
}

$aliases = @{
    "rec" = "record"
    "rep" = "report"
    "scr" = "script"
    "ann" = "annotate"
    "dif" = "diff"
}
foreach ($alias in $aliases.Keys) {
    $aliased = Expect-Success "alias $alias" @($alias, "--help")
    $canonical = Expect-Success "$($aliases[$alias]) --help" @($aliases[$alias], "--help")
    if ((Normalize-Text $aliased.Output) -ne (Normalize-Text $canonical.Output)) {
        $failures.Add("alias '$alias' does not resolve to '$($aliases[$alias])'")
    }
}

foreach ($compact in @(
    @("record", "-F99", "--help"),
    @("report", "--percent-limit=0", "--help"),
    @("trace", "-e", "sched", "--max=10", "--help")
)) {
    $result = Expect-Success ("compact options " + ($compact -join " ")) $compact
    if ($result.Output -notmatch '^Usage:') {
        $failures.Add("compact option form did not render help: $($compact -join ' ')")
    }
}

$targetHelp = Invoke-Wperf @("record", "--", "app.exe", "--help")
if ($targetHelp.Output -match 'Usage: wperf record') {
    $failures.Add("target '--help' was incorrectly intercepted by the wperf parser")
}
$markdown = Expect-Success "markdown help" @("help", "--format", "markdown")
if ($markdown.Output -notmatch '^# wperf Command Reference') {
    $failures.Add("markdown help is missing the command reference heading")
}
foreach ($command in $commands) {
    if ($markdown.Output -notmatch ('## `' + [regex]::Escape($command) + '`')) {
        $failures.Add("markdown help does not contain '$command'")
    }
}

if (-not (Test-Path -LiteralPath $ReferencePath)) {
    $failures.Add("generated command reference is missing: $ReferencePath")
} else {
    $expected = Normalize-Text ([IO.File]::ReadAllText($ReferencePath))
    $actual = Normalize-Text $markdown.Output
    if ($expected -ne $actual) {
        $failures.Add("checked-in command reference is stale; regenerate it")
    }
}

$internalTokens = @(
    "--internal-", "--pipe", "--nonce", "--guard-log", "--correlation",
    "--controller-pid", "--profile-interval", "--pmc-source-set"
)
foreach ($token in $internalTokens) {
    if ($general.Output -match [regex]::Escape($token)) {
        $failures.Add("general help exposes internal token '$token'")
    }
    if ($markdown.Output -match [regex]::Escape($token)) {
        $failures.Add("command reference exposes internal token '$token'")
    }
}

Expect-Exit "unknown command" 2 @("does-not-exist")
Expect-Exit "unknown report option" 2 @("report", "--definitely-invalid")
Expect-Exit "missing trace event" 2 @("trace", "-e")
Expect-Exit "conflicting report formats" 2 @("report", "--json", "--csv")
Expect-Exit "PMC frequency conflict" 2 @("record", "--pmc-source", "19", "-F", "999")

if ($failures.Count -gt 0) {
    $failures | ForEach-Object { Write-Error $_ }
    exit 1
}

Write-Output "cli conformance: PASS"