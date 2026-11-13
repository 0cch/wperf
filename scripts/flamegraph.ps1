[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$InputFile,

    [string]$OutputFile = "",
    [string]$SymbolPath = "",
    [int]$ProcessId = 0,
    [double]$PercentLimit = 0,
    [string]$Title = "wperf CPU profile",
    [string]$Wperf = "",
    [string]$FlameGraphScript = "",
    [string]$Perl = ""
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$inputPath = (Resolve-Path -LiteralPath $InputFile).Path

if ([string]::IsNullOrWhiteSpace($OutputFile)) {
    $OutputFile = Join-Path (Get-Location) "flamegraph.svg"
}
$outputPath = [IO.Path]::GetFullPath($OutputFile)
$outputDirectory = Split-Path -Parent $outputPath
if (-not [string]::IsNullOrWhiteSpace($outputDirectory)) {
    New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
}
$foldedPath = [IO.Path]::ChangeExtension($outputPath, ".folded")

if ([string]::IsNullOrWhiteSpace($Wperf)) {
    $candidate = Join-Path $repo "build\Release\wperf.exe"
    if (Test-Path -LiteralPath $candidate) {
        $Wperf = $candidate
    } else {
        $Wperf = (Get-Command wperf.exe -ErrorAction Stop).Source
    }
}
$Wperf = (Resolve-Path -LiteralPath $Wperf).Path

if ([string]::IsNullOrWhiteSpace($FlameGraphScript)) {
    $candidates = @(
        (Join-Path $repo "artifacts\FlameGraph\flamegraph.pl"),
        (Join-Path $repo "FlameGraph\flamegraph.pl")
    )
    if (-not [string]::IsNullOrWhiteSpace($env:FLAMEGRAPH_PL)) {
        $candidates = @($env:FLAMEGRAPH_PL) + $candidates
    }
    $FlameGraphScript = $candidates |
        Where-Object { Test-Path -LiteralPath $_ } |
        Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace($FlameGraphScript)) {
        throw "flamegraph.pl not found. Clone https://github.com/brendangregg/FlameGraph.git or pass -FlameGraphScript."
    }
}
$FlameGraphScript = (Resolve-Path -LiteralPath $FlameGraphScript).Path

if ([string]::IsNullOrWhiteSpace($Perl)) {
    $perlCandidates = @(
        (Get-Command perl.exe -ErrorAction SilentlyContinue | Select-Object -First 1).Source,
        "C:\Program Files\Git\usr\bin\perl.exe",
        "C:\msys64\usr\bin\perl.exe",
        "C:\Strawberry\perl\bin\perl.exe"
    ) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) -and (Test-Path -LiteralPath $_) }
    $Perl = $perlCandidates | Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace($Perl)) {
        throw "Perl not found. Install Perl or pass -Perl."
    }
}
$Perl = (Resolve-Path -LiteralPath $Perl).Path

$arguments = @("report", "-i", $inputPath, "--folded", "--quiet")
if (-not [string]::IsNullOrWhiteSpace($SymbolPath)) {
    $arguments += @("--symbol-path", (Resolve-Path -LiteralPath $SymbolPath).Path)
}
if ($ProcessId -gt 0) {
    $arguments += @("--pid", $ProcessId.ToString())
}
if ($PercentLimit -gt 0) {
    $arguments += @("--percent-limit", $PercentLimit.ToString([Globalization.CultureInfo]::InvariantCulture))
}

$errorFile = [IO.Path]::GetTempFileName()
try {
    $foldedLines = @(& $Wperf @arguments 2> $errorFile)
    $wperfExit = $LASTEXITCODE
    $wperfError = [IO.File]::ReadAllText($errorFile)
} finally {
    Remove-Item -LiteralPath $errorFile -Force -ErrorAction SilentlyContinue
}
if ($wperfExit -ne 0) {
    throw "wperf folded export failed with exit code $wperfExit`n$wperfError"
}
if ($wperfError.Trim().Length -ne 0) {
    Write-Warning $wperfError.Trim()
}
if ($foldedLines.Count -eq 0) {
    throw "wperf produced no folded stacks"
}

# flamegraph.pl on Windows may not strip CR from CRLF input. Write LF explicitly.
[IO.File]::WriteAllText(
    $foldedPath,
    (($foldedLines -join "`n") + "`n"),
    [Text.UTF8Encoding]::new($false))

$svgLines = @(& $Perl $FlameGraphScript --title $Title $foldedPath)
$flamegraphExit = $LASTEXITCODE
if ($flamegraphExit -ne 0) {
    throw "flamegraph.pl failed with exit code $flamegraphExit"
}
[IO.File]::WriteAllText(
    $outputPath,
    (($svgLines -join "`n") + "`n"),
    [Text.UTF8Encoding]::new($false))

$svg = [IO.File]::ReadAllText($outputPath)
if (-not $svg.Contains("<svg")) {
    throw "generated output is not an SVG document"
}

Write-Host "Folded stacks: $foldedPath"
Write-Host "Flame graph:   $outputPath"
Write-Host "Stacks:        $($foldedLines.Count)"