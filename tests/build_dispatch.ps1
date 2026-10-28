$ErrorActionPreference = 'Stop'
[System.Diagnostics.Process]::GetCurrentProcess().PriorityClass = 'BelowNormal'
$source = Split-Path $PSScriptRoot -Parent
$runtime = Join-Path $source 'devtools/portable-msvc'
$compiler = Join-Path $runtime 'VC/Tools/MSVC/14.44.35207'
$sdk = Join-Path $runtime 'Windows Kits/10'
$cmake = Join-Path $runtime 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
$ninja = Join-Path $runtime 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja'
$env:PATH = "$compiler/bin/Hostx64/x64;$sdk/bin/10.0.26100.0/x64;$ninja;$env:PATH"
$env:INCLUDE = "$compiler/include;$sdk/Include/10.0.26100.0/ucrt;$sdk/Include/10.0.26100.0/shared;$sdk/Include/10.0.26100.0/um;$sdk/Include/10.0.26100.0/winrt"
$env:LIB = "$compiler/lib/x64;$sdk/Lib/10.0.26100.0/ucrt/x64;$sdk/Lib/10.0.26100.0/um/x64"
$build = Join-Path $source 'build/worker-dispatch-20261017'
$headers = Join-Path $build 'ext/imgui'
New-Item -ItemType Directory -Force -Path $headers | Out-Null
Copy-Item -LiteralPath (Join-Path $source 'build/frame-timing-20261017/ext/imgui/imgui.h'), (Join-Path $source 'build/frame-timing-20261017/ext/imgui/imconfig.h') -Destination $headers
& $cmake -S $source -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release -DMGPU_BUILD_TESTS=ON 2>&1 | Tee-Object -FilePath (Join-Path $build 'configure.log')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $cmake --build $build --parallel 1 2>&1 | Tee-Object -FilePath (Join-Path $build 'build.log')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& (Join-Path (Split-Path $cmake) 'ctest.exe') --test-dir $build --output-on-failure 2>&1 | Tee-Object -FilePath (Join-Path $build 'tests.log')
exit $LASTEXITCODE
