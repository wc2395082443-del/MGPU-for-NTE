$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$game = 'C:/Program Files/Neverness To Everness/Client/WindowsNoEditor/HT/Binaries/Win64'
$candidate = Join-Path $root 'build/worker-dispatch-20261017/mgpu_bridge.addon64'
$target = Join-Path $game 'nvngx.dll_mgpu_bridge.addon64'
$config = Join-Path $game 'mgpu.ini'
$expected = 'C6B5A05AFABFB07D7532544DE2DF7EC3547944F168E666706CC115474E3A1462'
$previous = 'D8FF8285FBB085F01F3CBB499F70678A92AE76C8910E356CE0437A9C8F1E64F1'
function Assert-GameExited {
    foreach ($process in @(Get-Process HTGame -ErrorAction SilentlyContinue)) {
        if (-not $process.HasExited) { throw "Live HTGame PID $($process.Id); replacement refused" }
    }
}
Assert-GameExited
if ((Get-FileHash -LiteralPath $candidate -Algorithm SHA256).Hash -ne $expected) { throw 'Candidate hash mismatch' }
if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $previous) { throw 'Installed version changed; replacement refused' }
$configBefore = (Get-FileHash -LiteralPath $config -Algorithm SHA256).Hash
$backup = Join-Path $root ('deploy-backups/r330-deploy-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $backup | Out-Null
Copy-Item -LiteralPath $target,$config,(Join-Path $game 'ReShade.log') -Destination $backup
if ((Get-FileHash -LiteralPath (Join-Path $backup 'nvngx.dll_mgpu_bridge.addon64') -Algorithm SHA256).Hash -ne $previous) {
    throw 'Backup verification failed'
}
$staging = Join-Path $game 'nvngx.dll_mgpu_bridge.r330.pending'
if (-not (Test-Path -LiteralPath $staging)) {
    Copy-Item -LiteralPath $candidate -Destination $staging
}
if ((Get-FileHash -LiteralPath $staging -Algorithm SHA256).Hash -ne $expected) { throw 'Staging hash mismatch' }
Assert-GameExited
$replaceBackup = Join-Path $backup 'atomic-original.addon64'
[System.IO.File]::Replace([System.IO.Path]::GetFullPath($staging), [System.IO.Path]::GetFullPath($target), [System.IO.Path]::GetFullPath($replaceBackup))
$actual = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
$configAfter = (Get-FileHash -LiteralPath $config -Algorithm SHA256).Hash
$receipt = [ordered]@{
    version = 'R330 worker-owned aligned dispatch'
    timestamp = (Get-Date).ToString('o')
    backup = $backup
    target = $target
    before_sha256 = $previous
    candidate_sha256 = $expected
    installed_sha256 = $actual
    config_before_sha256 = $configBefore
    config_after_sha256 = $configAfter
    config_unchanged = $configBefore -eq $configAfter
    game_launched = $false
    runtime_improvement_verified = $false
}
$json = $receipt | ConvertTo-Json -Depth 4
[System.IO.File]::WriteAllText((Join-Path $root 'build/worker-dispatch-20261017/deployment-receipt.json'), $json, [System.Text.UTF8Encoding]::new($false))
Write-Output $json
if ($actual -ne $expected -or $configBefore -ne $configAfter) { throw 'Post-deployment verification failed' }
