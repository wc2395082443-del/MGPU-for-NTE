param(
    [Parameter(Mandatory = $true)][string]$NvidiaCore,
    [switch]$AllowSdkQuery,
    [switch]$InitializeFg
)

$ErrorActionPreference = 'Stop'
if (-not $AllowSdkQuery) {
    throw 'SDK query is opt-in. No DLL loaded. Use -AllowSdkQuery only after accepting a GPU driver query.'
}
$core = (Resolve-Path -LiteralPath $NvidiaCore).Path
$driverRoot = [IO.Path]::GetFullPath((Join-Path $env:WINDIR 'System32\DriverStore\FileRepository')) + '\'
if (-not $core.StartsWith($driverRoot, [StringComparison]::OrdinalIgnoreCase) -or
    [IO.Path]::GetFileName($core) -ne '_nvngx.dll') {
    throw 'Only the official NVIDIA core in the Windows DriverStore is accepted.'
}
$fg = Join-Path ([IO.Path]::GetDirectoryName($core)) 'nvngx_dlssg.dll'
$runtime = Join-Path ([IO.Path]::GetDirectoryName($core)) 'nvngx.dll'
foreach ($path in @($core, $fg, $runtime)) {
    $signature = Get-AuthenticodeSignature -LiteralPath $path
    if ($signature.Status -ne 'Valid' -or
        $signature.SignerCertificate.Subject -notmatch 'NVIDIA|Microsoft Windows Hardware Compatibility Publisher') {
        throw "Invalid driver file signature: $path"
    }
    Get-FileHash -LiteralPath $path -Algorithm SHA256 | Select-Object Path, Hash
}
if ($InitializeFg) {
    $auditedCore = '177604C12E78B49FEC5E15C25B056E9618D0C57F54FBD549F24E5F568C976B26'
    $auditedRuntime = '3D1A356CC31B039E1CA8E974B2A4192403F72A16D7AE18A8F738947E5D213EFE'
    if ((Get-FileHash -LiteralPath $core).Hash -ne $auditedCore -or
        (Get-FileHash -LiteralPath $runtime).Hash -ne $auditedRuntime) {
        throw 'Initialization ABI is audited only for the recorded driver runtime hashes. Re-audit after a driver update; a requirements-only query remains available.'
    }
}
$logDirectory = Join-Path $PSScriptRoot 'probe-logs'
[IO.Directory]::CreateDirectory($logDirectory) | Out-Null
$start = [Diagnostics.ProcessStartInfo]::new()
$start.FileName = Join-Path $PSScriptRoot 'capability_probe.exe'
$start.WorkingDirectory = $PSScriptRoot
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
$start.ArgumentList.Add('--allow-sdk-query')
$start.ArgumentList.Add($core)
$start.ArgumentList.Add($logDirectory)
if ($InitializeFg) { $start.ArgumentList.Add('--initialize-fg') }
$process = [Diagnostics.Process]::Start($start)
$stdout = $process.StandardOutput.ReadToEndAsync()
$stderr = $process.StandardError.ReadToEndAsync()
$timedOut = -not $process.WaitForExit(30000)
if ($timedOut) {
    $process.Kill($true)
    $process.WaitForExit()
}
$output = $stdout.GetAwaiter().GetResult()
$errorOutput = $stderr.GetAwaiter().GetResult()
$output
$errorOutput
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$report = [ordered]@{
    timestamp = (Get-Date).ToUniversalTime().ToString('o')
    initialize_fg = [bool]$InitializeFg
    timed_out = $timedOut
    exit_code = $process.ExitCode
    stdout = $output
    stderr = $errorOutput
    core_sha256 = (Get-FileHash -LiteralPath $core -Algorithm SHA256).Hash
    fg_sha256 = (Get-FileHash -LiteralPath $fg -Algorithm SHA256).Hash
    runtime_sha256 = (Get-FileHash -LiteralPath $runtime -Algorithm SHA256).Hash
    probe_sha256 = (Get-FileHash -LiteralPath $start.FileName -Algorithm SHA256).Hash
}
$report | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $logDirectory "probe-$stamp.json") -Encoding utf8
if ($timedOut) { throw 'SDK probe exceeded 30 seconds. Its isolated process tree was terminated; report preserved.' }
if ($process.ExitCode -ne 0) { throw "Probe did not complete successfully; exit code $($process.ExitCode). See preserved report." }
