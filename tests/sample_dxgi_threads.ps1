param(
    [Parameter(Mandatory=$true)][int]$GamePid,
    [ValidateRange(5,30)][int]$Seconds = 15,
    [Parameter(Mandatory=$true)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
[System.Diagnostics.Process]::GetCurrentProcess().PriorityClass = 'BelowNormal'
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class DxgiAuditNative {
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    [DllImport("kernel32.dll")] public static extern IntPtr OpenThread(uint access, bool inherit, uint thread);
    [DllImport("kernel32.dll")] public static extern int GetThreadDescription(IntPtr thread, out IntPtr description);
    [DllImport("kernel32.dll")] public static extern IntPtr LocalFree(IntPtr memory);
    [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr handle);
    [DllImport("ntdll.dll")] public static extern int NtQueryInformationThread(IntPtr thread, int kind, out IntPtr address, int bytes, IntPtr returned);
    public static uint ForegroundProcess() {
        uint process;
        GetWindowThreadProcessId(GetForegroundWindow(), out process);
        return process;
    }
}
'@
$deadline = [DateTime]::UtcNow.AddSeconds(60)
$ready = $null
while ([DateTime]::UtcNow -lt $deadline) {
    if ([DxgiAuditNative]::ForegroundProcess() -eq $GamePid) {
        if ($null -eq $ready) { $ready = [DateTime]::UtcNow }
        if (([DateTime]::UtcNow - $ready).TotalSeconds -ge 3) { break }
    } else { $ready = $null }
    Start-Sleep -Milliseconds 200
}
if ($null -eq $ready -or ([DateTime]::UtcNow - $ready).TotalSeconds -lt 3) { throw 'No stable foreground game' }
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Output directory already exists' }
$output = (New-Item -ItemType Directory -Path $OutputDirectory).FullName
$trace = Join-Path $output 'dxgi.etl'
$session = 'MGPU-DxgiAudit-' + [Guid]::NewGuid().ToString('N')
$game = 'C:/Program Files/Neverness To Everness/Client/WindowsNoEditor/HT/Binaries/Win64'
$hashes = Get-FileHash -LiteralPath (Join-Path $game 'mgpu.ini'),(Join-Path $game 'nvngx.dll_mgpu_bridge.addon64')
$started = Get-Date
$observations = [System.Collections.Generic.List[object]]::new()
& logman.exe start $session -p '{CA11C036-0102-4A2D-A6AD-F03CFED5D3C9}' 0x2 0 -o $trace -f bincirc -max 16 -bs 64 -nb 16 32 -ets
if ($LASTEXITCODE -ne 0) { throw 'DXGI trace start failed' }
try {
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    while ($watch.Elapsed.TotalSeconds -lt $Seconds) {
        $foreground = [DxgiAuditNative]::ForegroundProcess()
        $observations.Add([pscustomobject]@{timestamp=(Get-Date).ToString('o');foreground_pid=$foreground;game_foreground=($foreground -eq $GamePid)})
        Start-Sleep -Milliseconds 200
    }
} finally {
    & logman.exe stop $session -ets
    if ($LASTEXITCODE -ne 0) { throw "Trace stop failed: $session" }
}
$ended = Get-Date
$observations | Export-Csv -LiteralPath (Join-Path $output 'foreground.csv') -NoTypeInformation
$events = @(Get-WinEvent -Path $trace -Oldest -FilterXPath "*[System[(EventID=42 or EventID=43) and Execution[@ProcessID='$GamePid']]]")
$rows = foreach ($event in $events) {
    [xml]$document = $event.ToXml()
    $values = [ordered]@{timestamp=$event.TimeCreated.ToString('o');event_id=$event.Id;thread_id=$event.ThreadId}
    foreach ($field in $document.Event.EventData.Data) { $values[$field.Name] = $field.'#text' }
    [pscustomobject]$values
}
$rows | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'events.json') -Encoding UTF8
$process = Get-Process -Id $GamePid
$modules = @($process.Modules | ForEach-Object { [pscustomobject]@{name=$_.ModuleName;base=$_.BaseAddress.ToInt64();bytes=$_.ModuleMemorySize} })
$threads = foreach ($threadId in ($events.ThreadId | Sort-Object -Unique)) {
    $handle = [DxgiAuditNative]::OpenThread(0x0040 -bor 0x0800, $false, $threadId)
    $description = ''
    $address = [IntPtr]::Zero
    $queryStatus = -1
    if ($handle -ne [IntPtr]::Zero) {
        try {
            $pointer = [IntPtr]::Zero
            if ([DxgiAuditNative]::GetThreadDescription($handle, [ref]$pointer) -eq 0) {
                $description = [Runtime.InteropServices.Marshal]::PtrToStringUni($pointer)
                [void][DxgiAuditNative]::LocalFree($pointer)
            }
            $queryStatus = [DxgiAuditNative]::NtQueryInformationThread($handle, 9, [ref]$address, [IntPtr]::Size, [IntPtr]::Zero)
        } finally { [void][DxgiAuditNative]::CloseHandle($handle) }
    }
    $module = $modules | Where-Object { $address.ToInt64() -ge $_.base -and $address.ToInt64() -lt $_.base + $_.bytes } | Select-Object -First 1
    [pscustomobject]@{thread_id=$threadId;description=$description;start_address=('0x{0:X}' -f $address.ToInt64());query_status=$queryStatus;start_module=$module.name;present_start_count=@($rows | Where-Object { $_.thread_id -eq $threadId -and $_.event_id -eq 42 }).Count}
}
$threads | Export-Csv -LiteralPath (Join-Path $output 'threads.csv') -NoTypeInformation
Copy-Item -LiteralPath (Join-Path $game 'ReShade.log') -Destination (Join-Path $output 'after.log')
$after = Get-FileHash -LiteralPath (Join-Path $game 'mgpu.ini'),(Join-Path $game 'nvngx.dll_mgpu_bridge.addon64')
$metadata = [ordered]@{pid=$GamePid;start=$started.ToString('o');end=$ended.ToString('o');session=$session;stopped=$true;seconds=$Seconds;foreground=@($observations | Where-Object game_foreground).Count;observations=$observations.Count;before=$hashes;after=$after;unchanged=(($hashes.Hash -join ',') -eq ($after.Hash -join ','));threads=$threads}
$metadata | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $output 'sample-window.json') -Encoding UTF8
$metadata | ConvertTo-Json -Depth 6
