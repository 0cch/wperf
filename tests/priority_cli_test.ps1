[CmdletBinding()]
param([Parameter(Mandatory)][string]$Wperf,[Parameter(Mandatory)][string]$FixtureDirectory,[Parameter(Mandatory)][string]$OutputDirectory)
$ErrorActionPreference='Stop'
function Invoke-Case([string[]]$Arguments,[int]$ExpectedExit=0) {
    $lines=& $Wperf @Arguments 2>$null
    if($LASTEXITCODE -ne $ExpectedExit) { throw "Expected exit $ExpectedExit for $Arguments, got $LASTEXITCODE" }
    return ($lines -join "`n")
}
$cpu=Join-Path $FixtureDirectory 'cpu-stack.etl'
$io=Join-Path $FixtureDirectory 'io.etl'
$cpuPid=(Get-Content -LiteralPath ($cpu + ".pid") -Raw).Trim()
$stream=(Invoke-Case @('report','-i',$cpu,'-p',$cpuPid,'--stream','--json','--metadata','--quiet'))|ConvertFrom-Json
$sum=($stream.rows|Measure-Object samples -Sum).Sum
if($sum -le 0 -or $sum -gt $stream.quality.samples) { throw 'Stream quality does not reflect consumed samples' }
if($stream.quality.stacks_loaded -ne $false -or $stream.quality.stack_assessment -ne 'not-loaded' -or $stream.quality.missing_stacks -ne 0) { throw 'Omitted stacks were incorrectly classified as missing' }
$empty=(Invoke-Case @('report','-i',$io,'--kind','io','--json','-p','4294967294') 5)|ConvertFrom-Json
if($empty.file_io -ne 0 -or $empty.disk_io -ne 0 -or @($empty.files).Count -ne 0) { throw 'I/O PID selection was ignored' }
$emptyCpu=(Invoke-Case @('report','-i',$io,'--kind','io','--json','--cpu','4294967294') 5)|ConvertFrom-Json
if($emptyCpu.file_io -ne 0) { throw 'I/O CPU selection was ignored' }
Invoke-Case @('report','-i',$io,'--kind','io','--max-missing-stacks','0') 2|Out-Null
Invoke-Case @('report','-i',$io,'--kind','io','--scope','kernel') 2|Out-Null
Invoke-Case @('report','-i',$io,'--kind','pmc','--folded') 2|Out-Null
Invoke-Case @('report','--flamegraph','--folded') 2|Out-Null
Invoke-Case @('report','--flamegraph','--json') 2|Out-Null
Invoke-Case @('report','--title','orphan title') 2|Out-Null
Invoke-Case @('flamegraph') 2|Out-Null
Invoke-Case @('report','-i',$cpu,'-p',$cpuPid,'--stream','--max-missing-stacks','0') 2|Out-Null
Invoke-Case @('report','--weight','bogus') 2|Out-Null
Invoke-Case @('diff','--match','bogus') 2|Out-Null
Invoke-Case @('record','--pmc-source','19','--pmc-counters','19') 2|Out-Null
New-Item -ItemType Directory -Force -Path $OutputDirectory|Out-Null
$output=Join-Path $OutputDirectory 'script-output.csv'
$stdout=Invoke-Case @('script','-i',$cpu,'-p',$cpuPid,'--limit','2','--csv','-F','pid,period','-o',$output)
if($stdout.Length -ne 0 -or -not (Test-Path -LiteralPath $output)) { throw 'script -o did not route output to the requested file' }
$bytes=[IO.File]::ReadAllBytes($output)
$text=[Text.Encoding]::UTF8.GetString($bytes)
if($text.Contains("`r") -or ($bytes.Length -ge 3 -and $bytes[0]-eq 239 -and $bytes[1]-eq 187 -and $bytes[2]-eq 191)) { throw 'script file must be UTF-8 without BOM and LF-only' }
if(@($text.TrimEnd() -split "`n").Count -ne 3) { throw 'script limit/header mismatch' }
$stat=(Invoke-Case @('stat','-p',"$PID",'--duration','20','--per-thread','--json'))|ConvertFrom-Json
if(@($stat.threads).Count -eq 0 -or $stat.thread_counter_scope -ne 'observation-delta') { throw 'Thread JSON output is missing observation deltas' }
$csv=Invoke-Case @('stat','-p',"$PID",'--duration','20','--per-thread','--csv')
$csvRows=@($csv|ConvertFrom-Csv)
if(@($csvRows|Where-Object row_type -eq 'thread').Count -eq 0) { throw 'Thread CSV rows are missing' }
foreach($line in ($csv -split "`n")) { if(($line -split ',').Count -ne 14) { throw 'Thread CSV column count mismatch' } }
$statText=Invoke-Case @('stat','-p',"$PID",'--duration','20','--per-thread')
if($statText -notmatch 'observation deltas') { throw 'Thread text output has no delta scope' }
Write-Output 'priority CLI behavior: PASS'
