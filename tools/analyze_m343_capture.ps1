param(
    [Parameter(Mandatory=$true)][string]$CaptureDirectory,
    [Parameter(Mandatory=$true)][string]$Python,
    [string]$RawExporter
)
$ErrorActionPreference = 'Stop'
[Diagnostics.Process]::GetCurrentProcess().PriorityClass = 'BelowNormal'
$directory = (Resolve-Path -LiteralPath $CaptureDirectory).Path
$window = Get-Content -Raw -LiteralPath (Join-Path $directory 'sample-window.json') | ConvertFrom-Json
if (-not $window.valid_collection_envelope) { throw 'Invalid capture envelope; no causality claim' }
$log = Join-Path $directory 'after.log'
$compact = Join-Path $directory 'compact-report.json'
$fine = Join-Path $directory 'fine-report.json'
$copy = Join-Path $directory 'copy-analysis.json'
$queue = Join-Path $directory 'queue-causality.json'
& $Python (Join-Path $PSScriptRoot '../tests/analyze_m337b_jobs.py') $log $compact
if ($LASTEXITCODE -ne 0) { throw 'Native command trace invalid' }
& $Python (Join-Path $PSScriptRoot 'analyze_m339_native_work.py') $log $fine
if ($LASTEXITCODE -ne 0) { throw 'Native operation trace invalid' }
& $Python (Join-Path $PSScriptRoot 'analyze_m341_copy.py') $log | Set-Content -LiteralPath $copy -Encoding UTF8
if ($LASTEXITCODE -ne 0) { throw 'COPY trace invalid' }
& $Python (Join-Path $PSScriptRoot 'analyze_m343_queue_trace.py') (Join-Path $directory 'queue-trace.bin') $queue --compact $compact --copy $copy --log $log
if ($LASTEXITCODE -ne 0) { throw 'API/native join invalid; no cause classification' }
if ($window.etw) {
    if (-not $RawExporter -or -not (Test-Path -LiteralPath $RawExporter)) { throw 'Supply the matching compiled M343 ETL exporter' }
    $export = Join-Path $directory 'etl-decoded'
    & (Join-Path $PSScriptRoot 'export_m343_etw.ps1') -Trace (Join-Path $directory 'queues.etl') -OutputDirectory $export -RawExporter $RawExporter
    & $Python (Join-Path $PSScriptRoot 'audit_m343_system_events.py') $export $queue (Join-Path $directory 'system-coverage.json')
    if ($LASTEXITCODE -ne 0) { throw 'ETW identity/coverage audit failed; API evidence retained, do not infer GPU idle' }
    $system = Get-Content -Raw -LiteralPath (Join-Path $directory 'system-coverage.json') | ConvertFrom-Json
    if ($system.scheduling_log_events -gt 0) {
        $causality = Get-Content -Raw -LiteralPath $queue | ConvertFrom-Json
        $firstSeal = $causality.gaps[0].seal
        $queueIdentity = $causality.gaps[0].queue
        $adapter = @($causality.objects | Where-Object object -eq $queueIdentity)[0].value
        $arguments = @((Join-Path $PSScriptRoot 'analyze_etl_scheduler.py'),'--raw',(Join-Path $export 'raw.csv'),
            '--xml',(Join-Path $export 'gpu.xml'),'--log',$log,'--output',(Join-Path $directory 'scheduler-analysis.json'),
            '--session',$causality.header.session,'--first-seal',$firstSeal,'--count','5','--game-pid',$window.pid,'--adapter-luid',$adapter,
            '--clock-samples',(Join-Path $directory 'scheduler-clock.json'))
        & $Python @arguments 2>&1 | Set-Content -LiteralPath (Join-Path $directory 'scheduler-validation.log') -Encoding UTF8
        $valid = $LASTEXITCODE -eq 0
        [ordered]@{decoder_passed=$valid; note='A passing context decoder still cannot separate queues sharing a context; inspect mapping and coverage before attribution'} |
            ConvertTo-Json | Set-Content -LiteralPath (Join-Path $directory 'scheduler-validation.json') -Encoding UTF8
    }
}
Write-Output 'Five-frame API/native gap evidence saved. Missing scheduler or fence evidence remains unknown.'
