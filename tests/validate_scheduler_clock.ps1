param(
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [Parameter(Mandatory=$true)][string]$Python,
    [Parameter(Mandatory=$true)][string]$ProfilePath,
    [Parameter(Mandatory=$true)][ValidatePattern('^0x[0-9a-fA-F]{1,16}$')][string]$GameLuid,
    [Parameter(Mandatory=$true)][ValidatePattern('^0x[0-9a-fA-F]{1,16}$')][string]$NeuralLuid
)
$ErrorActionPreference='Stop'
$output=[IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $output) { throw 'Clock validation directory must be new' }
New-Item -ItemType Directory -Path $output | Out-Null
$profile=(Resolve-Path -LiteralPath $ProfilePath).Path
$probe=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../tools/query_scheduler_clock.py')).Path
$instance='M338bClock-'+[Guid]::NewGuid().ToString('N')
$arguments=@('-u',('"'+$probe+'"'),'--luid',$GameLuid,'--luid',$NeuralLuid,'--count','201','--interval','0.05','--output',('"'+(Join-Path $output 'scheduler-clock.json')+'"'))
$process=$null
$started=$false
$stopped=$false
$errorMessage=$null
$begin=[Diagnostics.Stopwatch]::GetTimestamp()
try {
    $process=Start-Process -FilePath $Python -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $output 'clock-stdout.txt') -RedirectStandardError (Join-Path $output 'clock-stderr.txt')
    Start-Sleep -Milliseconds 250
    if ($process.HasExited) { throw 'Clock helper exited before ETW start' }
    & wpr.exe -start ($profile+'!MGPUFrame') -instancename $instance 2>&1 | Set-Content -Encoding utf8 (Join-Path $output 'trace-start.txt')
    if ($LASTEXITCODE -ne 0) { throw 'Own WPR session failed to start' }
    $started=$true
    $begin=[Diagnostics.Stopwatch]::GetTimestamp()
    Start-Sleep -Seconds 3
} catch {
    $errorMessage=$_.Exception.Message
} finally {
    $end=[Diagnostics.Stopwatch]::GetTimestamp()
    if ($started) {
        & wpr.exe -stop (Join-Path $output 'queues.etl') -instancename $instance 2>&1 | Set-Content -Encoding utf8 (Join-Path $output 'trace-stop.txt')
        $stopped=$LASTEXITCODE -eq 0
        if (-not $stopped) { & wpr.exe -cancel -instancename $instance 2>&1 | Set-Content -Encoding utf8 (Join-Path $output 'own-trace-cleanup.txt') }
    }
    $clockOk=$false
    if ($process) {
        if (-not $process.WaitForExit(20000)) {
            $process.Kill()
            $process.WaitForExit()
        } else { $clockOk=$process.ExitCode -eq 0 }
        $process.Dispose()
    }
    [ordered]@{
        purpose='Read-only scheduler clock-domain validation, not a game FPS/frame breakdown'
        start_qpc=$begin
        end_qpc=$end
        qpc_frequency=[Diagnostics.Stopwatch]::Frequency
        internal_game_sampling_triggered=$false
        game_launched=$false
        game_terminated=$false
        dll_or_configuration_modified=$false
        gpu_workload_created=$false
        wpr_instance=$instance
        trace_stopped=$stopped
        clock_helper_complete=$clockOk
        error=$errorMessage
    } | ConvertTo-Json | Set-Content -Encoding utf8 (Join-Path $output 'pilot-metadata.json')
}
if ($errorMessage -or -not $stopped -or -not $clockOk) { throw 'Clock pilot incomplete; inspect preserved metadata' }
Write-Output $output
