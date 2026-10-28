param()

$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$dependencies = @(
    @{
        repository = 'crosire/reshade'
        revision = '18deaa52de0c425a78b329e9cb3c497281cd00ec'
        directory = 'ext/reshade'
        paths = @('include/reshade.hpp', 'include/reshade_api.hpp',
            'include/reshade_api_device.hpp', 'include/reshade_api_format.hpp',
            'include/reshade_api_pipeline.hpp', 'include/reshade_api_resource.hpp',
            'include/reshade_events.hpp', 'include/reshade_overlay.hpp', 'LICENSE.md')
    },
    @{
        repository = 'NVIDIAGameWorks/Streamline'
        revision = 'v2.10.3'
        directory = 'ext/streamline'
        paths = @('include/sl.h', 'include/sl_consts.h', 'include/sl_core_types.h',
            'include/sl_core_api.h', 'include/sl_result.h', 'include/sl_struct.h',
            'include/sl_version.h', 'include/sl_appidentity.h',
            'include/sl_device_wrappers.h', 'license.txt')
    },
    @{
        repository = 'ocornut/imgui'
        revision = '3912b3d9a9c1b3f17431aebafd86d2f40ee6e59c'
        directory = 'build/ext/imgui'
        paths = @('imgui.h', 'imconfig.h', 'LICENSE.txt')
    }
)

foreach ($dependency in $dependencies) {
    $api = "https://api.github.com/repos/$($dependency.repository)"
    $tree = Invoke-RestMethod -Uri "$api/git/trees/$($dependency.revision)?recursive=1" -TimeoutSec 20
    if ($tree.truncated) { throw 'Incomplete dependency tree.' }
    foreach ($path in $dependency.paths) {
        $entry = @($tree.tree | Where-Object { $_.path -eq $path -and $_.type -eq 'blob' })
        if ($entry.Count -ne 1) { throw "Pinned dependency missing: $($dependency.repository)/$path" }
        $target = Join-Path (Join-Path $root $dependency.directory) ([IO.Path]::GetFileName($path))
        if (Test-Path -LiteralPath $target) {
            $bytes = [IO.File]::ReadAllBytes($target)
        } else {
            $response = Invoke-RestMethod -Uri "$api/git/blobs/$($entry[0].sha)" -TimeoutSec 30
            $bytes = [Convert]::FromBase64String($response.content)
        }
        $hash = [Security.Cryptography.IncrementalHash]::CreateHash([Security.Cryptography.HashAlgorithmName]::SHA1)
        try {
            $hash.AppendData([Text.Encoding]::ASCII.GetBytes("blob $($bytes.Length)`0"))
            $hash.AppendData($bytes)
            $actual = [Convert]::ToHexString($hash.GetHashAndReset()).ToLowerInvariant()
        } finally { $hash.Dispose() }
        if ($actual -ne $entry[0].sha) { throw "Dependency integrity mismatch: $target" }
        if (-not (Test-Path -LiteralPath $target)) {
            [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target)) | Out-Null
            [IO.File]::WriteAllBytes($target, $bytes)
        }
        Write-Output "Verified $($dependency.repository) $path ($($bytes.Length) bytes)"
    }
}
