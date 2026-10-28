$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$zig = Join-Path $root 'devtools/ziglang/zig.exe'
$bridge = Join-Path $root 'build/parameter_bridge.obj'
[IO.Directory]::CreateDirectory((Join-Path $root 'build')) | Out-Null
$oldCache = $env:ZIG_GLOBAL_CACHE_DIR
$process = [Diagnostics.Process]::GetCurrentProcess()
$oldAffinity = $process.ProcessorAffinity
try {
    $process.ProcessorAffinity = [IntPtr]3
    $env:ZIG_GLOBAL_CACHE_DIR = Join-Path $root 'build/zig-cache'
    & $zig cc -target x86_64-windows-msvc -std=c++17 -O2 -g0 -nostdlib -nostdinc `
        -isystem (Join-Path $root 'devtools/ziglang/lib/include') `
        -fno-exceptions -fno-rtti -Wall -Wextra -Werror -Wno-unused-command-line-argument `
        -c (Join-Path $PSScriptRoot 'parameter_bridge.cpp') -o $bridge
    if ($LASTEXITCODE -ne 0) { throw 'MSVC-ABI bridge compilation failed.' }
    $floatMarker = Join-Path $root 'build/msvc_float_marker.obj'
    & $zig cc -target x86_64-windows-gnu -O2 -g0 -Wall -Wextra -Werror `
        -c (Join-Path $PSScriptRoot 'msvc_float_marker.c') -o $floatMarker
    if ($LASTEXITCODE -ne 0) { throw 'Floating-point compiler marker compilation failed.' }
    & $zig c++ -target x86_64-windows-gnu -std=c++17 -O0 -g0 -fms-extensions `
        -DWIN32_LEAN_AND_MEAN -DNOMINMAX -Wall -Wextra -Werror `
        (Join-Path $PSScriptRoot 'capability_probe.cpp') $bridge $floatMarker `
        -ldxgi -ldxguid -ld3d12 -municode -o (Join-Path $PSScriptRoot 'capability_probe.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Probe linkage failed.' }
    $testBridge = Join-Path $root 'build/parameter_bridge_test.obj'
    & $zig cc -target x86_64-windows-msvc -std=c++17 -O2 -g0 -nostdlib -nostdinc `
        -isystem (Join-Path $root 'devtools/ziglang/lib/include') `
        -fno-exceptions -fno-rtti -Wall -Wextra -Werror -Wno-unused-command-line-argument `
        -c (Join-Path $PSScriptRoot 'parameter_bridge_test.cpp') -o $testBridge
    if ($LASTEXITCODE -ne 0) { throw 'MSVC-ABI fake parameter object compilation failed.' }
    & $zig c++ -target x86_64-windows-gnu -std=c++17 -O0 -g0 -fms-extensions `
        -DWIN32_LEAN_AND_MEAN -DNOMINMAX -Wall -Wextra -Werror `
        (Join-Path $PSScriptRoot 'stage_api_test.cpp') (Join-Path $PSScriptRoot 'dlss_fg_stage.cpp') `
        $bridge $testBridge $floatMarker -ldxguid -o (Join-Path $PSScriptRoot 'stage_api_test.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Stage API test linkage failed.' }
} finally {
    $process.ProcessorAffinity = $oldAffinity
    $env:ZIG_GLOBAL_CACHE_DIR = $oldCache
}
