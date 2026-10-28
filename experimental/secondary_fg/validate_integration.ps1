$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$vc = Join-Path $root 'devtools/portable-msvc/VC/Tools/MSVC/14.44.35207'
$sdk = Join-Path $root 'devtools/portable-msvc/Windows Kits/10'
$cmakeDir = Join-Path $root 'devtools/portable-msvc/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin'
$ninjaDir = Join-Path $root 'devtools/portable-msvc/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja'
$env:INCLUDE = @(
    (Join-Path $vc 'include'),
    (Join-Path $sdk 'Include/10.0.26100.0/ucrt'),
    (Join-Path $sdk 'Include/10.0.26100.0/shared'),
    (Join-Path $sdk 'Include/10.0.26100.0/um'),
    (Join-Path $sdk 'Include/10.0.26100.0/winrt')
) -join ';'
$env:LIB = @(
    (Join-Path $vc 'lib/x64'),
    (Join-Path $sdk 'Lib/10.0.26100.0/ucrt/x64'),
    (Join-Path $sdk 'Lib/10.0.26100.0/um/x64')
) -join ';'
$env:PATH = @((Join-Path $vc 'bin/Hostx64/x64'), $cmakeDir, $ninjaDir,
    (Join-Path $sdk 'bin/10.0.26100.0/x64'), $env:PATH) -join ';'

& (Join-Path $ninjaDir 'ninja.exe') -C (Join-Path $root 'build/main-fresh') -j 2
if ($LASTEXITCODE -ne 0) { throw 'Main add-on build failed' }
& (Join-Path $cmakeDir 'ctest.exe') --test-dir (Join-Path $root 'build/main-fresh') --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Main CPU tests failed' }
& (Join-Path $cmakeDir 'cmake.exe') -S $PSScriptRoot -B (Join-Path $root 'build/fg-proto') -G Ninja `
    "-DCMAKE_CXX_COMPILER=$(Join-Path $vc 'bin/Hostx64/x64/cl.exe')" -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { throw 'FG configure failed' }
& (Join-Path $ninjaDir 'ninja.exe') -C (Join-Path $root 'build/fg-proto') -j 2
if ($LASTEXITCODE -ne 0) { throw 'FG build failed' }
& (Join-Path $cmakeDir 'ctest.exe') --test-dir (Join-Path $root 'build/fg-proto') --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'FG CPU tests failed' }
Write-Output 'PASS: add-on and isolated FG builds, CPU-only tests. No game launch or deployment.'
