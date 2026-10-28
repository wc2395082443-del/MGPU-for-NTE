param(
    [Parameter(Mandatory = $true)][int]$GamePid,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [Parameter(Mandatory = $true)][ValidateSet('Clean', 'Bridged')][string]$Mode,
    [Parameter(Mandatory = $true)][string]$Scene,
    [ValidatePattern('^[A-Fa-f0-9]{64}$')][string]$ExpectedDllHash,
    [ValidateRange(3, 30)][int]$Seconds = 10,
    [switch]$InternalTiming,
    [switch]$Etw,
    [switch]$Telemetry,
    [switch]$SchedulerClock,
    [string]$ClockProbePath,
    [string]$Python,
    [string]$ProfilePath,
    [switch]$Capture
)
$ErrorActionPreference = 'Stop'
[Diagnostics.Process]::GetCurrentProcess().PriorityClass = 'BelowNormal'
if ($Etw -and $Seconds -gt 15) { throw 'ETW is capped at 15 seconds; use separate longer internal-only captures.' }
if ($Mode -eq 'Clean' -and $InternalTiming) { throw 'Clean mode cannot enable bridge instrumentation.' }
if ($Telemetry -and (-not $Python -or -not (Test-Path -LiteralPath $Python))) { throw 'Telemetry requires an existing Python executable path.' }
if ($SchedulerClock -and (-not $Etw -or -not $Python -or -not (Test-Path -LiteralPath $Python))) { throw 'Scheduler clock requires ETW and an existing Python executable path.' }
if ($SchedulerClock) {
    if ($Mode -ne 'Bridged') { throw 'Current fresh-clock collector requires the verified bridged game/adapter selection log.' }
    if ($Seconds -ne 3) { throw 'Fresh scheduler-clock detailed capture is currently limited to three seconds.' }
    if (-not $ClockProbePath) { $ClockProbePath = Join-Path $PSScriptRoot '../tools/query_scheduler_clock.py' }
    if (-not (Test-Path -LiteralPath $ClockProbePath)) { throw 'Scheduler clock probe is missing.' }
}
if ($Mode -eq 'Bridged' -and -not $ExpectedDllHash) { throw 'Bridged capture requires the exact expected loaded DLL hash.' }
$game = Get-Process -Id $GamePid
if ($game.ProcessName -ne 'HTGame') { throw 'Expected HTGame.exe.' }
$gameStart = $game.StartTime.ToUniversalTime().ToString('o')
$directory = Split-Path $game.Path -Parent
$modules = @($game.Modules | Select-Object ModuleName, FileName, BaseAddress, ModuleMemorySize)
$bridge = @($modules | Where-Object ModuleName -like '*mgpu_bridge*')
if ($Mode -eq 'Clean' -and $bridge.Count -ne 0) { throw 'Clean capture has a bridge loaded.' }
if ($Mode -eq 'Bridged' -and $bridge.Count -ne 1) { throw 'Expected exactly one bridge module.' }
$files = @($game.Path)
if ($Mode -eq 'Bridged') {
    if ((Get-FileHash -LiteralPath $bridge[0].FileName).Hash -ne $ExpectedDllHash) { throw 'Loaded module path hash differs from expected candidate.' }
    $files += $bridge[0].FileName
    $files += Join-Path $directory 'mgpu.ini'
}
$before = @(Get-FileHash -LiteralPath $files)
$clockLuids = @()
if ($SchedulerClock) {
    $selection = @(Select-String -LiteralPath (Join-Path $directory 'ReShade.log') -Pattern 'SELECTED adapter\[\d+\] luid=0x([0-9a-fA-F]{8})-0x([0-9a-fA-F]{8}).*game luid=0x([0-9a-fA-F]{8})-0x([0-9a-fA-F]{8})')
    if ($selection.Count -eq 0) { throw 'Cannot establish current neural/game LUIDs for the clock probe.' }
    $selectionMatch = $selection[-1].Matches[0]
    $clockLuids = @(('0x' + $selectionMatch.Groups[3].Value + $selectionMatch.Groups[4].Value), ('0x' + $selectionMatch.Groups[1].Value + $selectionMatch.Groups[2].Value))
    if ($clockLuids.Count -ne 2 -or $clockLuids[0] -notmatch '^0x[0-9a-fA-F]{16}$' -or $clockLuids[1] -notmatch '^0x[0-9a-fA-F]{16}$') { throw 'Invalid clock LUID extraction' }
}
$profile = if ($ProfilePath) { (Resolve-Path -LiteralPath $ProfilePath).Path } else { Join-Path $PSScriptRoot 'gpu_frame_measurement.wprp' }
$profileDetails = & wpr.exe -profiledetails ($profile + '!MGPUFrame')
if ($LASTEXITCODE -ne 0) { throw 'WPR profile validation failed.' }
if ($Etw) {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'ETW requires an elevated terminal.' }
}
$outputPath = [IO.Path]::GetFullPath($OutputDirectory)
$drive = [IO.DriveInfo]::new([IO.Path]::GetPathRoot($outputPath))
if ($drive.AvailableFreeSpace -lt 2GB) { throw 'At least 2 GiB free space required.' }
$preflight = [ordered]@{
    mode = $Mode; pid = $GamePid; game_path = $game.Path; game_started_utc = $gameStart
    scene = $Scene; seconds = $Seconds; internal_timing = [bool]$InternalTiming
    etw = [bool]$Etw; etw_buffers_mib = ([xml](Get-Content -Raw -LiteralPath $profile)).WindowsPerformanceRecorder.Profiles.ChildNodes |
        Where-Object { $_.LocalName -in 'SystemCollector', 'EventCollector' } |
        ForEach-Object { [int]$_.BufferSize.Value * [int]$_.Buffers.Value / 1024 } | Measure-Object -Sum | Select-Object -ExpandProperty Sum
    stackwalk = $false
    telemetry = [bool]$Telemetry; python = $Python
    scheduler_clock = [bool]$SchedulerClock; clock_probe = $ClockProbePath; clock_luids = $clockLuids
    hashes = $before; modules = $modules; profile = $profile
    profile_hash = (Get-FileHash -LiteralPath $profile).Hash
    limitations = @('No exclusive whole-game GPU time without validated ETW queue/adapter mapping and packet coverage.',
        'File hashes do not prove live panel parameters. Record panel and game settings separately.',
        'No game launches, process termination, DLL writes or configuration changes.')
}
if (-not $Capture) { $preflight | ConvertTo-Json -Depth 7; return }
if (Test-Path -LiteralPath $outputPath) { throw 'Output directory must be new.' }
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class FrameMeasurementWindow {
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    public static uint Foreground() {
        uint process;
        GetWindowThreadProcessId(GetForegroundWindow(), out process);
        return process;
    }
}
'@
$deadline = [DateTime]::UtcNow.AddSeconds(30)
while ([FrameMeasurementWindow]::Foreground() -ne $GamePid) {
    if ([DateTime]::UtcNow -ge $deadline) { throw 'Game not foreground; nothing started.' }
    Start-Sleep -Milliseconds 200
}
$control = $null
if ($InternalTiming) {
    $control = [Threading.EventWaitHandle]::OpenExisting("Local\MGPUFrameTiming.$GamePid")
    if ($control.WaitOne(0)) { $control.Dispose(); throw 'An internal measurement is already active.' }
}
New-Item -ItemType Directory -Path $outputPath | Out-Null
$preflight | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath (Join-Path $outputPath 'preflight.json') -Encoding UTF8
$profileDetails | Set-Content -LiteralPath (Join-Path $outputPath 'profile-details.txt') -Encoding UTF8
Copy-Item -LiteralPath $profile -Destination (Join-Path $outputPath 'profile.wprp')
$modules | Export-Csv -LiteralPath (Join-Path $outputPath 'modules.csv') -NoTypeInformation
$instance = 'MGPUFrame-' + [Guid]::NewGuid().ToString('N')
$traceStarted = $false
$traceStopped = -not $Etw
$observations = [Collections.Generic.List[object]]::new()
$started = [DateTimeOffset]::Now
$startQpc = [Diagnostics.Stopwatch]::GetTimestamp()
$errorMessage = $null
$telemetryProcess = $null
$clockProcess = $null
try {
    if ($SchedulerClock) {
        $clockArguments = @('-u', ('"' + [IO.Path]::GetFullPath($ClockProbePath) + '"'), '--luid', $clockLuids[0], '--luid', $clockLuids[1], '--count', '241', '--interval', '0.05', '--output', ('"' + (Join-Path $outputPath 'scheduler-clock.json') + '"'))
        $clockProcess = Start-Process -FilePath $Python -ArgumentList $clockArguments -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $outputPath 'clock-stdout.txt') -RedirectStandardError (Join-Path $outputPath 'clock-stderr.txt')
        Start-Sleep -Milliseconds 250
        if ($clockProcess.HasExited) { throw 'Scheduler clock process exited before ETW started.' }
    }
    if ($Etw) {
        & wpr.exe -start ($profile + '!MGPUFrame') -instancename $instance 2>&1 |
            Set-Content -LiteralPath (Join-Path $outputPath 'trace-start.txt') -Encoding UTF8
        if ($LASTEXITCODE -ne 0) { throw 'WPR start failed; no other session will be cancelled.' }
        $traceStarted = $true
    }
    if ($Telemetry) {
        $telemetryArguments = @('-u', ('"' + (Join-Path $PSScriptRoot 'sample_frame_environment.py') + '"'),
            '--pid', $GamePid, '--seconds', ($Seconds + 1), '--output', ('"' + (Join-Path $outputPath 'environment') + '"'))
        $telemetryProcess = Start-Process -FilePath $Python -ArgumentList $telemetryArguments -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $outputPath 'environment-stdout.txt') -RedirectStandardError (Join-Path $outputPath 'environment-stderr.txt')
    }
    $started = [DateTimeOffset]::Now
    $startQpc = [Diagnostics.Stopwatch]::GetTimestamp()
    if ($control) { $control.Set() | Out-Null }
    $watch = [Diagnostics.Stopwatch]::StartNew()
    while ($watch.Elapsed.TotalSeconds -lt $Seconds) {
        $game.Refresh()
        if ($game.HasExited) { throw 'Game exited during collection.' }
        $observations.Add([pscustomobject]@{
            timestamp = [DateTimeOffset]::Now.ToString('o')
            qpc = [Diagnostics.Stopwatch]::GetTimestamp()
            foreground_pid = [FrameMeasurementWindow]::Foreground()
        })
        Start-Sleep -Milliseconds 100
    }
} catch {
    $errorMessage = $_.Exception.Message
} finally {
    $stopQpc = [Diagnostics.Stopwatch]::GetTimestamp()
    if ($control) {
        try { $control.Reset() | Out-Null } catch { $errorMessage = $_.Exception.Message }
        finally { $control.Dispose() }
    }
    $ended = [DateTimeOffset]::Now
    if ($traceStarted) {
        $statusOutput = & wpr.exe -status collectors -details -instancename $instance 2>&1
        try {
            $stopOutput = & wpr.exe -stop (Join-Path $outputPath 'queues.etl') -instancename $instance 2>&1
            $traceStopped = $LASTEXITCODE -eq 0
        } finally {
            if (-not $traceStopped) {
                $cleanupOutput = & wpr.exe -cancel -instancename $instance 2>&1
            }
        }
        $statusOutput | Set-Content -LiteralPath (Join-Path $outputPath 'trace-status.txt') -Encoding UTF8
        $stopOutput | Set-Content -LiteralPath (Join-Path $outputPath 'trace-stop.txt') -Encoding UTF8
        if ($cleanupOutput) { $cleanupOutput | Set-Content -LiteralPath (Join-Path $outputPath 'own-session-cleanup.txt') -Encoding UTF8 }
    }
    $telemetryOk = -not $Telemetry
    $clockOk = -not $SchedulerClock
    if ($clockProcess) {
        if (-not $clockProcess.WaitForExit(20000)) {
            $clockProcess.Kill()
            $clockProcess.WaitForExit()
        } else {
            $clockOk = $clockProcess.ExitCode -eq 0 -and (Test-Path -LiteralPath (Join-Path $outputPath 'scheduler-clock.json'))
        }
        $clockProcess.Dispose()
    }
    if ($telemetryProcess) {
        if (-not $telemetryProcess.WaitForExit(10000)) {
            $telemetryProcess.Kill()
            $telemetryProcess.WaitForExit()
        } else {
            $telemetryStatusPath = Join-Path $outputPath 'environment/status.json'
            if (Test-Path -LiteralPath $telemetryStatusPath) {
                $telemetryStatus = Get-Content -LiteralPath $telemetryStatusPath -Raw | ConvertFrom-Json
                $telemetryOk = $telemetryProcess.ExitCode -eq 0 -and $telemetryStatus.complete
            }
        }
        $telemetryProcess.Dispose()
    }
    Start-Sleep -Seconds 2
    $after = @(Get-FileHash -LiteralPath $files -ErrorAction SilentlyContinue)
    $unchanged = ($before.Hash -join ',') -eq ($after.Hash -join ',')
    $allForeground = $observations.Count -gt 0 -and @($observations | Where-Object foreground_pid -ne $GamePid).Count -eq 0
    $metadata = [ordered]@{
        mode = $Mode; pid = $GamePid; game_started_utc = $gameStart; scene = $Scene
        start = $started.ToString('o'); end = $ended.ToString('o')
        anchor_qpc = $startQpc; qpc_frequency = [Diagnostics.Stopwatch]::Frequency
        stop_qpc = $stopQpc
        duration_requested_s = $Seconds; etw = [bool]$Etw; internal_timing = [bool]$InternalTiming
        instance = $instance; trace_stopped = $traceStopped; hashes_before = $before; hashes_after = $after
        unchanged = $unchanged; all_foreground = $allForeground; error = $errorMessage
        telemetry_complete = $telemetryOk
        scheduler_clock_complete = $clockOk
        valid_collection_envelope = $unchanged -and $allForeground -and $traceStopped -and $telemetryOk -and $clockOk -and -not $errorMessage
        causal_attribution_ready = $false
    }
    $metadata | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath (Join-Path $outputPath 'sample-window.json') -Encoding UTF8
    $observations | Export-Csv -LiteralPath (Join-Path $outputPath 'foreground.csv') -NoTypeInformation
    if ($Mode -eq 'Bridged') {
        Copy-Item -LiteralPath (Join-Path $directory 'mgpu.ini') -Destination (Join-Path $outputPath 'mgpu.ini.snapshot')
        Copy-Item -LiteralPath (Join-Path $directory 'ReShade.log') -Destination (Join-Path $outputPath 'after.log')
    }
    Get-ChildItem -LiteralPath $outputPath -File -Recurse | Where-Object Name -ne 'evidence-index.csv' | Get-FileHash |
        Select-Object Path, Algorithm, Hash | Export-Csv -LiteralPath (Join-Path $outputPath 'evidence-index.csv') -NoTypeInformation
}
if (-not $metadata.valid_collection_envelope) { throw 'Collection incomplete; inspect sample-window.json. No performance conclusion permitted.' }
Write-Output $outputPath
