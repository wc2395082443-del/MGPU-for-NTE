param(
    [Parameter(Mandatory=$true)][int]$GamePid,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [Parameter(Mandatory=$true)][string]$Scene,
    [Parameter(Mandatory=$true)][ValidatePattern('^[A-Fa-f0-9]{64}$')][string]$ExpectedDllHash,
    [Parameter(Mandatory=$true)][string]$Python,
    [switch]$ApiOnly,
    [switch]$Capture
)
$ErrorActionPreference = 'Stop'
$arguments = @{
    GamePid=$GamePid; OutputDirectory=$OutputDirectory; Scene=$Scene; Mode='Bridged';
    ExpectedDllHash=$ExpectedDllHash; Seconds=3; InternalTiming=$true; Python=$Python;
    ProfilePath=(Join-Path $PSScriptRoot '../tests/gpu_queue_causality.wprp'); Capture=[bool]$Capture
}
if (-not $ApiOnly) { $arguments.Etw=$true; $arguments.SchedulerClock=$true }
& (Join-Path $PSScriptRoot 'collect_m339_frame_measurement.ps1') @arguments
if (-not $Capture) { return }
$outputPath = [IO.Path]::GetFullPath($OutputDirectory)
$logPath = Join-Path $outputPath 'after.log'
$starts = @(Select-String -LiteralPath $logPath -Pattern ('\[M336\] kind=start session=(\d+) pid='+$GamePid+'(?:\s|$)'))
if ($starts.Count -ne 1) { throw 'Expected exactly one fresh detailed session; no recapture performed' }
$session = $starts[0].Matches[0].Groups[1].Value
$trace = Join-Path ([IO.Path]::GetTempPath()) "MGPUQueueTrace.$GamePid.$session.bin"
$deadline = [DateTime]::UtcNow.AddSeconds(10)
while (-not (Test-Path -LiteralPath $trace)) {
    if ([DateTime]::UtcNow -gt $deadline) { throw 'Queue trace export missing/incomplete; retain evidence, do not recapture this process' }
    Start-Sleep -Milliseconds 100
}
$copy = Join-Path $outputPath 'queue-trace.bin'
Copy-Item -LiteralPath $trace -Destination $copy
if ((Get-FileHash -LiteralPath $copy).Hash -ne (Get-FileHash -LiteralPath $trace).Hash) { throw 'Queue trace snapshot mismatch' }
& $Python (Join-Path $PSScriptRoot 'analyze_m343_queue_trace.py') $copy (Join-Path $outputPath 'queue-trace.json')
if ($LASTEXITCODE -ne 0) { throw 'API trace failed validation; no completeness or cause claim allowed' }
$report = Get-Content -Raw -LiteralPath (Join-Path $outputPath 'queue-trace.json') | ConvertFrom-Json
if ($report.header.session.ToString() -ne $session) { throw 'Export session mismatch' }
[ordered]@{
    session=$session; queue_trace_valid=$report.valid; queue_trace_sha256=(Get-FileHash -LiteralPath $copy).Hash;
    etw_requested=(-not $ApiOnly); kernel_execution_validated=$false; cpu_wait_attribution_validated=$false;
    all_gpu_events_visible=$false; queue_context_mapping_validated=$false;
    next='Validate native trace, exact batch/job links, ETW loss/rundown and clock coverage before classifying gaps'
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $outputPath 'causality-coverage.json') -Encoding UTF8
Write-Output 'API trace captured. ETW presence alone does not establish RUNNING or wait attribution.'
