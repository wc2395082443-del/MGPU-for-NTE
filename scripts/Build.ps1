param([string]$BuildDirectory='_build/nte',[string]$StreamlineSDK='_dl/streamline-sdk',[int]$Parallel=2)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
if(-not [IO.Path]::IsPathRooted($BuildDirectory)){$BuildDirectory=Join-Path $root $BuildDirectory}
if(-not [IO.Path]::IsPathRooted($StreamlineSDK)){$StreamlineSDK=Join-Path $root $StreamlineSDK}
if(-not(Test-Path -LiteralPath (Join-Path $StreamlineSDK 'include/sl_dlss_g.h'))){throw 'Run scripts/Fetch-Dependencies.ps1 first'}
$main=Join-Path $BuildDirectory 'main'
$helper=Join-Path $BuildDirectory 'helper'
& cmake -S $root -B $main -G Ninja -DCMAKE_BUILD_TYPE=Release -DMGPU_BUILD_TESTS=ON
if($LASTEXITCODE){throw 'Main configure failed'}
& cmake --build $main --parallel $Parallel
if($LASTEXITCODE){throw 'Main build failed'}
& ctest --test-dir $main --output-on-failure
if($LASTEXITCODE){throw 'Main regression failed'}
& cmake -S (Join-Path $root 'experimental/official_fg') -B $helper -G Ninja -DCMAKE_BUILD_TYPE=Release "-DSTREAMLINE_SDK=$StreamlineSDK"
if($LASTEXITCODE){throw 'Helper configure failed'}
& cmake --build $helper --parallel $Parallel
if($LASTEXITCODE){throw 'Helper build failed'}
& ctest --test-dir $helper --output-on-failure
if($LASTEXITCODE){throw 'Helper regression failed'}
'PASS: source build and CTest; live game performance and visual acceptance are separate'
