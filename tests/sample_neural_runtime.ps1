param(
    [ValidateRange(15, 120)][int]$Seconds = 20,
    [Parameter(Mandatory = $true)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
[Diagnostics.Process]::GetCurrentProcess().PriorityClass = 'BelowNormal'
$game = 'C:\Program Files\Neverness To Everness\Client\WindowsNoEditor\HT\Binaries\Win64'
$log = Join-Path $game 'ReShade.log'
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class NeuralRuntimeForeground {
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
}
'@
Copy-Item -LiteralPath (Join-Path $game 'mgpu.ini') -Destination (Join-Path $OutputDirectory 'mgpu.ini.snapshot')
$startLines = Get-Content -LiteralPath $log -Tail 1200
$startLines | Select-String -Pattern '\[R326-PIPE\]|\[R327-HISTORY\]|\[R328-TRANSFER\]|\[R323-ALIGN\]|\[R323-WAIT\]|\[R313-PRESENT\]|GAME PRESENT cadence' |
    ForEach-Object { $_.Line } | Set-Content -LiteralPath (Join-Path $OutputDirectory 'before.log') -Encoding UTF8
$samples = [Collections.Generic.List[object]]::new()
$start = [DateTimeOffset]::Now
$watch = [Diagnostics.Stopwatch]::StartNew()
while ($watch.Elapsed.TotalSeconds -lt $Seconds) {
    $timestamp = [DateTimeOffset]::Now.ToString('o')
    [uint32]$foregroundId = 0
    [void][NeuralRuntimeForeground]::GetWindowThreadProcessId([NeuralRuntimeForeground]::GetForegroundWindow(), [ref]$foregroundId)
    $process = Get-Process -Name HTGame -ErrorAction SilentlyContinue | Where-Object { -not $_.HasExited -and $_.Threads.Count -gt 0 } | Select-Object -First 1
    $gpuRows = & nvidia-smi --query-gpu=index,name,utilization.gpu,utilization.memory,power.draw,temperature.gpu,clocks.current.graphics,clocks.current.memory --format=csv,noheader,nounits
    if ($LASTEXITCODE -ne 0) { throw 'nvidia-smi sample failed.' }
    foreach ($row in ($gpuRows | ConvertFrom-Csv -Header index,name,gpu_util,memory_util,power_w,temp_c,graphics_mhz,memory_mhz)) {
        $samples.Add([pscustomobject]@{
            timestamp = $timestamp
            elapsed_s = $watch.Elapsed.TotalSeconds
            index = [int]$row.index
            name = $row.name.Trim()
            gpu_util = [int]$row.gpu_util
            memory_util = [int]$row.memory_util
            power_w = [double]$row.power_w
            temp_c = [int]$row.temp_c
            graphics_mhz = [int]$row.graphics_mhz
            memory_mhz = [int]$row.memory_mhz
            game_process = if ($process) { $process.Id } else { 0 }
            game_cpu_s = if ($process) { $process.CPU } else { 0 }
            foreground_process = $foregroundId
            game_foreground = $process -and $foregroundId -eq $process.Id
        })
    }
    Start-Sleep -Milliseconds 1000
}
$end = [DateTimeOffset]::Now
$samples | Export-Csv -LiteralPath (Join-Path $OutputDirectory 'samples.csv') -NoTypeInformation -Encoding UTF8
Copy-Item -LiteralPath $log -Destination (Join-Path $OutputDirectory 'after.log')
[ordered]@{
    start = $start.ToString('o')
    end = $end.ToString('o')
    elapsed_s = $watch.Elapsed.TotalSeconds
    samples = $samples.Count
    game_dll_sha256 = (Get-FileHash -LiteralPath (Join-Path $game 'nvngx.dll_mgpu_bridge.addon64')).Hash
    config_sha256 = (Get-FileHash -LiteralPath (Join-Path $game 'mgpu.ini')).Hash
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'sample-window.json') -Encoding UTF8
$samples | Group-Object name | ForEach-Object {
    [pscustomobject]@{
        gpu = $_.Name
        samples = $_.Count
        utilization_mean = [Math]::Round(($_.Group.gpu_util | Measure-Object -Average).Average, 2)
        power_mean_w = [Math]::Round(($_.Group.power_w | Measure-Object -Average).Average, 2)
        game_foreground_samples = @($_.Group | Where-Object game_foreground).Count
    }
} | Format-Table -AutoSize
Write-Output ('EVIDENCE=' + (Resolve-Path -LiteralPath $OutputDirectory).Path)
