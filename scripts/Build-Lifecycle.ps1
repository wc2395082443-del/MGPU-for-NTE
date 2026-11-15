param([string]$BuildDirectory='_build/preview4',[string]$StreamlineSDK='_dl/streamline-sdk',[int]$Parallel=2)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
if(-not [IO.Path]::IsPathRooted($BuildDirectory)){$BuildDirectory=Join-Path $root $BuildDirectory}
if(-not [IO.Path]::IsPathRooted($StreamlineSDK)){$StreamlineSDK=Join-Path $root $StreamlineSDK}
$main=Join-Path $BuildDirectory 'main'
$helper=Join-Path $BuildDirectory 'helper'
$headers=Join-Path $main 'ext/imgui'
if(Test-Path -LiteralPath (Join-Path $root '_build/nte/main/ext/imgui/imgui.h')){
    New-Item -ItemType Directory -Path $headers -Force|Out-Null
    foreach($name in @('imgui.h','imconfig.h')){Copy-Item -LiteralPath (Join-Path $root ('_build/nte/main/ext/imgui/'+$name)) -Destination $headers}
}
& cmake -S $root -B $main -G Ninja -DCMAKE_BUILD_TYPE=Release -DMGPU_BUILD_TESTS=ON
if($LASTEXITCODE){throw 'Main configure failed'}
$targets=@('mgpu_bridge','mgpu_streamline_route_audit_test','mgpu_early_input_backpressure_test','mgpu_source_admission_gate_test','mgpu_source_credit_serial_control','mgpu_admission_audit_test','mgpu_present_backpressure_test','mgpu_nr_audit_policy_test','mgpu_source_fg_guard_test','mgpu_source_fg_options_test','mgpu_aligned_frame_policy_test','mgpu_neural_pipeline_test','mgpu_dualpass_pipeline_test','mgpu_nr_preset_test')
& cmake --build $main --parallel $Parallel --target $targets
if($LASTEXITCODE){throw 'Main focused build failed'}
& ctest --test-dir $main --output-on-failure -R '^mgpu_((streamline_route_audit|early_input_backpressure|source_admission_gate|admission_audit|present_backpressure|nr_audit_policy|source_fg_guard|source_fg_options|aligned_frame_policy|neural_pipeline|dualpass_pipeline|nr_preset)_test|source_credit_serial_control_rejected)$'
if($LASTEXITCODE){throw 'Main focused regression failed'}
& cmake -S (Join-Path $root 'experimental/official_fg') -B $helper -G Ninja -DCMAKE_BUILD_TYPE=Release -DMGPU_FG_GPU_AUDIT=ON "-DSTREAMLINE_SDK=$StreamlineSDK"
if($LASTEXITCODE){throw 'Helper configure failed'}
& cmake --build $helper --parallel $Parallel
if($LASTEXITCODE){throw 'Helper build failed'}
& ctest --test-dir $helper --output-on-failure -R 'official_fg_gpu_audit_warp|official_output_trace|official_output_visibility|official_output_liveness|official_startup_source_policy|official_helper_measurement_disabled|official_source_history|official_reflex_contract|shared_frame_contract|official_output_window|official_fg_mode_policy'
if($LASTEXITCODE){throw 'Helper focused regression failed'}
'PASS: focused lifecycle/FG regression; full suites and live game acceptance reported separately'
