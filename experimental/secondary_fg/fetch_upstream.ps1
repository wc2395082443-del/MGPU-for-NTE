param([int]$MaxNewFiles = 60)

$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$revision = '6bb54cf0d006b1d4e8f18eaba9fca891e352f75d'
$repository = 'https://api.github.com/repos/maohgad-web/Neural-coprocessor'
$tree = Invoke-RestMethod -Uri "$repository/git/trees/${revision}?recursive=1" -TimeoutSec 20
if ($tree.truncated) { throw 'Refusing an incomplete source tree.' }
$files = @($tree.tree | Where-Object {
    $_.type -eq 'blob' -and ($_.path.StartsWith('src/') -or
        $_.path -in @('.github/workflows/build.yml', 'LICENSE', 'THIRD_PARTY.md'))
} | Sort-Object @{Expression = { if ($_.path.EndsWith('.hpp')) { 0 } else { 1 } }}, path)
$count = 0
foreach ($file in $files) {
    $target = [IO.Path]::GetFullPath((Join-Path $root $file.path))
    if (-not $target.StartsWith($root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Source tree path escaped the development directory.'
    }
    if (Test-Path -LiteralPath $target) { continue }
    if ($count -ge $MaxNewFiles) { break }
    $response = Invoke-WebRequest -Uri "$repository/git/blobs/$($file.sha)" -TimeoutSec 20
    $blob = $response.Content | ConvertFrom-Json
    $bytes = [Convert]::FromBase64String($blob.content)
    $prefix = [Text.Encoding]::ASCII.GetBytes("blob $($bytes.Length)`0")
    $hash = [Security.Cryptography.IncrementalHash]::CreateHash([Security.Cryptography.HashAlgorithmName]::SHA1)
    try {
        $hash.AppendData($prefix)
        $hash.AppendData($bytes)
        $actual = [Convert]::ToHexString($hash.GetHashAndReset()).ToLowerInvariant()
    } finally { $hash.Dispose() }
    if ($actual -ne $file.sha) { throw "Blob integrity mismatch: $($file.path)" }
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target)) | Out-Null
    [IO.File]::WriteAllBytes($target, $bytes)
    $count++
    Write-Output "Fetched $($file.path) ($($bytes.Length) bytes, pinned Git blob verified)"
    $remaining = @($response.Headers['X-RateLimit-Remaining'])[0]
    if ($null -ne $remaining -and [int]$remaining -lt 5) {
        Write-Warning 'Stopping before the public API quota is exhausted; rerun after its reset.'
        break
    }
}
$missing = @($files | Where-Object { -not (Test-Path -LiteralPath (Join-Path $root $_.path)) })
Write-Output "New files: $count; still missing: $($missing.Count). Existing files were never overwritten."
