[CmdletBinding()]
param(
    [string]$Configuration = "Release",
    [string]$OutputDirectory = ""
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$build = Join-Path $repo "build"
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $repo "dist"
}
$cmake = "C:\Program Files\CMake\bin\cmake.exe"

& $cmake -S $repo -B $build -G "Visual Studio 17 2022" -A x64
if ($LASTEXITCODE -ne 0) { throw "configure failed" }
& $cmake --build $build --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { throw "build failed" }
& 'C:\Program Files\CMake\bin\ctest.exe' --test-dir $build -C $Configuration --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "tests failed" }

$source = Join-Path $build "$Configuration\wperf.exe"
$output = Join-Path $OutputDirectory "wperf.exe"
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
Copy-Item -LiteralPath $source -Destination $output -Force
$version = & $output --version
$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $output).Hash
@(
    "wperf release",
    "configuration=$Configuration",
    "version=$version",
    "sha256=$hash",
    "built_at=$((Get-Date).ToString('o'))"
) | Out-File -LiteralPath (Join-Path $OutputDirectory "RELEASE.txt") -Encoding utf8
Write-Host "Release output: $output"
Write-Host "Version: $version"
Write-Host "SHA256: $hash"
