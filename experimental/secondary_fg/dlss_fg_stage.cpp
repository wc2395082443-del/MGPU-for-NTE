#include "dlss_fg_stage.hpp"
#include "parameter_bridge.hpp"

namespace mgpu::secondary_fg
{
namespace
{
bool same_adapter(ID3D12DeviceChild *object, LUID expected)
{
    if (object == nullptr) return false;
    ID3D12Device *device = nullptr;
    if (FAILED(object->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void **>(&device))))
        return false;
    const auto actual = device->GetAdapterLuid();
    device->Release();
    return actual.LowPart == expected.LowPart && actual.HighPart == expected.HighPart;
}

void transition(ID3D12GraphicsCommandList *commands, const texture_binding &binding,
                D3D12_RESOURCE_STATES destination, bool restore = false)
{
    if (binding.state == destination) return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = binding.resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = restore ? destination : binding.state;
    barrier.Transition.StateAfter = restore ? binding.state : destination;
    commands->ResourceBarrier(1, &barrier);
}

bool valid_texture(ID3D12Resource *resource, unsigned width, unsigned height)
{
    if (resource == nullptr) return false;
    const auto desc = resource->GetDesc();
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.Width == width &&
           desc.Height == height && desc.DepthOrArraySize == 1 && desc.MipLevels == 1 &&
           desc.SampleDesc.Count == 1;
}

bool fence_completed(ID3D12Fence *fence, std::uint64_t value)
{
    if (fence == nullptr) return false;
    const auto completed = fence->GetCompletedValue();
    return completed != UINT64_MAX && completed >= value;
}

bool valid_matrix(const float (&matrix)[4][4])
{
    bool nonzero = false;
    for (const auto &row : matrix)
        for (float value : row)
        {
            if (!std::isfinite(value)) return false;
            nonzero = nonzero || value != 0.0f;
        }
    return nonzero;
}

bool valid_camera(const NVSDK_NGX_DLSSG_Opt_Eval_Params &camera)
{
    if (!valid_matrix(camera.cameraViewToClip) || !valid_matrix(camera.clipToCameraView) ||
        !valid_matrix(camera.clipToLensClip) || !valid_matrix(camera.clipToPrevClip) ||
        !valid_matrix(camera.prevClipToClip)) return false;
    for (unsigned axis = 0; axis < 3; ++axis)
        if (!std::isfinite(camera.cameraPos[axis]) || !std::isfinite(camera.cameraUp[axis]) ||
            !std::isfinite(camera.cameraRight[axis]) || !std::isfinite(camera.cameraFwd[axis]))
            return false;
    for (unsigned axis = 0; axis < 2; ++axis)
        if (!std::isfinite(camera.jitterOffset[axis]) || !std::isfinite(camera.cameraPinholeOffset[axis]))
            return false;
    return std::isfinite(camera.cameraNear) && std::isfinite(camera.cameraFar) &&
           std::isfinite(camera.cameraFOV) && std::isfinite(camera.cameraAspectRatio) &&
           camera.cameraNear > 0.0f && camera.cameraFar > camera.cameraNear &&
           camera.cameraFOV > 0.0f && camera.cameraFOV < 3.1415927f &&
           camera.cameraAspectRatio > 0.0f;
}
}

NVSDK_NGX_Result dlss_fg_stage::check_capability(NVSDK_NGX_Parameter *capabilities, unsigned multiplier)
{
    if (capabilities == nullptr || parameters_ != nullptr || handle_ != nullptr || recorded_)
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    maximum_generated_frames_ = 1;
    capability_reported_ = false;
    int available = 0;
    if (NVSDK_NGX_FAILED(probe_parameter_get_int(capabilities, NVSDK_NGX_Parameter_FrameGeneration_Available, &available)) ||
        available == 0) return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    unsigned maximum = 0;
    capability_reported_ = !NVSDK_NGX_FAILED(probe_parameter_get_uint(capabilities,
        NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, &maximum));
    maximum_generated_frames_ = supported_generated_frames(capability_reported_, maximum);
    return supports_multiplier(multiplier, maximum_generated_frames_)
        ? NVSDK_NGX_Result_Success : NVSDK_NGX_Result_FAIL_FeatureNotSupported;
}

NVSDK_NGX_Result dlss_fg_stage::create(ID3D12Device *device, ID3D12GraphicsCommandList *commands,
                                      NVSDK_NGX_Parameter *capabilities, const ngx_api &api,
                                      const configuration &config)
{
    const auto invalid = NVSDK_NGX_Result_FAIL_InvalidParameter;
    if (parameters_ != nullptr || device == nullptr || capabilities == nullptr ||
        config.width == 0 || config.height == 0 || config.guide_width == 0 || config.guide_height == 0 ||
        config.format == DXGI_FORMAT_UNKNOWN || api.allocate == nullptr ||
        api.destroy == nullptr || api.create == nullptr ||
        api.evaluate == nullptr || api.release == nullptr || !same_adapter(commands, config.adapter_luid))
        return invalid;
    const auto adapter = device->GetAdapterLuid();
    if (adapter.LowPart != config.adapter_luid.LowPart || adapter.HighPart != config.adapter_luid.HighPart)
        return invalid;
    const auto capability_result = check_capability(capabilities, config.multiplier);
    if (NVSDK_NGX_FAILED(capability_result)) return capability_result;
    api_ = api;
    config_ = config;
    auto result = api_.allocate(&parameters_);
    if (NVSDK_NGX_FAILED(result)) return result;
    if (parameters_ == nullptr) return NVSDK_NGX_Result_FAIL_InvalidParameter;

    const parameter_writer parameters(parameters_);
    parameters.Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
    parameters.Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
    parameters.Set(NVSDK_NGX_Parameter_Width, config.width);
    parameters.Set(NVSDK_NGX_Parameter_Height, config.height);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFormat, static_cast<unsigned>(config.format));
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_InternalWidth, config.guide_width);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_InternalHeight, config.guide_height);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_DynamicResolution, 0u);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_UserInterfaceRecompositionEnabled, 0u);
    recorded_ = true;
    return api_.create(commands, NVSDK_NGX_Feature_FrameGeneration, parameters_, &handle_);
}

NVSDK_NGX_Result dlss_fg_stage::record(ID3D12GraphicsCommandList *commands,
                                      const frame_resources &frame, decision &result)
{
    result = {};
    if (handle_ == nullptr || recorded_ || evaluation_pending_ || !submitted_ || fence_ == nullptr ||
        !fence_completed(fence_, fence_value_) || !same_adapter(commands, config_.adapter_luid))
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    const auto candidate = history_.inspect(frame.contract);
    result.reason = candidate.reason;
    if (!candidate.evaluate) { history_.clear(); return NVSDK_NGX_Result_FAIL_InvalidParameter; }
    if (frame.camera_available && !valid_camera(frame.camera))
    {
        result = {};
        result.reason = rejection::invalid_camera;
        history_.clear();
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    }
    if (frame.generated_count == 0 || frame.generated_count > maximum_generated_frames_ ||
        frame.generated_count != config_.multiplier - 1u)
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    const texture_binding bindings[] = {frame.neural_color, frame.depth, frame.motion,
                                        frame.disable_interpolation};
    for (unsigned index = 0; index < 4; ++index)
    {
        if (!same_adapter(bindings[index].resource, config_.adapter_luid))
            return NVSDK_NGX_Result_FAIL_InvalidParameter;
        for (unsigned previous = 0; previous < index; ++previous)
            if (bindings[index].resource == bindings[previous].resource)
                return NVSDK_NGX_Result_FAIL_InvalidParameter;
    }
    if (!valid_texture(frame.neural_color.resource, config_.width, config_.height) ||
        !valid_texture(frame.depth.resource, config_.guide_width, config_.guide_height) ||
        !valid_texture(frame.motion.resource, config_.guide_width, config_.guide_height))
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    for (unsigned output = 0; output < frame.generated_count; ++output)
    {
        const auto &binding = frame.interpolated[output];
        if (!same_adapter(binding.resource, config_.adapter_luid) ||
            !valid_texture(binding.resource, config_.width, config_.height))
            return NVSDK_NGX_Result_FAIL_InvalidParameter;
        for (unsigned previous = 0; previous < output; ++previous)
            if (binding.resource == frame.interpolated[previous].resource)
                return NVSDK_NGX_Result_FAIL_InvalidParameter;
        for (const auto &input : bindings)
            if (binding.resource == input.resource)
                return NVSDK_NGX_Result_FAIL_InvalidParameter;
    }
    const auto output_desc = frame.interpolated[0].resource->GetDesc();
    const auto hint_desc = frame.disable_interpolation.resource->GetDesc();
    for (unsigned index = 0; index < 3; ++index)
        if (bindings[index].resource->GetDesc().Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)
            return NVSDK_NGX_Result_FAIL_InvalidParameter;
    for (unsigned output = 0; output < frame.generated_count; ++output)
        if ((frame.interpolated[output].resource->GetDesc().Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) ||
            frame.interpolated[output].resource->GetDesc().Format != config_.format ||
            !(frame.interpolated[output].resource->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
            return NVSDK_NGX_Result_FAIL_InvalidParameter;
    if (frame.neural_color.resource->GetDesc().Format != config_.format ||
        output_desc.Format != config_.format ||
        !(output_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ||
        hint_desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || hint_desc.Width < 4 ||
        !(hint_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
        return NVSDK_NGX_Result_FAIL_InvalidParameter;

    const parameter_writer parameters(parameters_);
    camera_ = frame.camera;
    camera_.multiFrameCount = frame.generated_count;
    camera_.multiFrameIndex = 1;
    camera_.reset = candidate.reset;
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_Backbuffer, frame.neural_color.resource);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_Depth, frame.depth.resource);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_MVecs, frame.motion.resource);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_HUDLess, static_cast<ID3D12Resource *>(nullptr));
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_UI, static_cast<ID3D12Resource *>(nullptr));
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_UIAlpha, static_cast<ID3D12Resource *>(nullptr));
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_BidirectionalDistortionField, static_cast<ID3D12Resource *>(nullptr));
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_OutputReal, static_cast<ID3D12Resource *>(nullptr));
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_OutputDisableInterpolation, frame.disable_interpolation.resource);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID,
                     static_cast<unsigned long long>(frame.contract.frame_id));
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameCount, frame.generated_count);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_MvecScaleX, frame.contract.normalized_motion_scale_x);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_MvecScaleY, frame.contract.normalized_motion_scale_y);
    if (frame.camera_available)
    {
        parameters.Set(NVSDK_NGX_DLSSG_Parameter_CameraViewToClip, static_cast<void *>(camera_.cameraViewToClip));
        parameters.Set(NVSDK_NGX_DLSSG_Parameter_ClipToCameraView, static_cast<void *>(camera_.clipToCameraView));
        parameters.Set(NVSDK_NGX_DLSSG_Parameter_ClipToLensClip, static_cast<void *>(camera_.clipToLensClip));
        parameters.Set(NVSDK_NGX_DLSSG_Parameter_ClipToPrevClip, static_cast<void *>(camera_.clipToPrevClip));
        parameters.Set(NVSDK_NGX_DLSSG_Parameter_PrevClipToClip, static_cast<void *>(camera_.prevClipToClip));
        parameters.Set(NVSDK_NGX_DLSSG_Parameter_JitterOffsetX, camera_.jitterOffset[0]);
        parameters.Set(NVSDK_NGX_DLSSG_Parameter_JitterOffsetY, camera_.jitterOffset[1]);
        parameters.Set(NVSDK_NGX_DLSSG_Parameter_CameraPinholeOffsetX, camera_.cameraPinholeOffset[0]);
        parameters.Set(NVSDK_NGX_DLSSG_Parameter_CameraPinholeOffsetY, camera_.cameraPinholeOffset[1]);
        parameters.Set(NVSDK_NGX_DLSSG_Parameter_CameraPosX, camera_.cameraPos[0]);
        parameters.Set(NVSDK_NGX_DLSSG_Parameter_CameraPosY, camera_.cameraPos[1]);
        parameters.Set(NVSDK_NGX_DLSSG_Parameter_CameraPosZ, camera_.cameraPos[2]);
        parameters.Set(NVSDK_NGX_DLSSG_Parameter_CameraUpX, camera_.cameraUp[0]);
        parameters.Set(NVSDK_NGX_DLSSG_Parameter_CameraUpY, camera_.cameraUp[1]);
        parameters.Set(NVSDK_NGX_DLSSG_Parameter_CameraUpZ, camera_.cameraUp[2]);
        parameters.Set(NVSDK_NGX_DLSSG_Parameter_CameraRightX, camera_.cameraRight[0]);
        parameters.Set(NVSDK_NGX_DLSSG_Parameter_CameraRightY, camera_.cameraRight[1]);
        parameters.Set(NVSDK_NGX_DLSSG_Parameter_CameraRightZ, camera_.cameraRight[2]);
        parameters.Set(NVSDK_NGX_DLSSG_Parameter_CameraFwdX, camera_.cameraFwd[0]);
        parameters.Set(NVSDK_NGX_DLSSG_Parameter_CameraFwdY, camera_.cameraFwd[1]);
        parameters.Set(NVSDK_NGX_DLSSG_Parameter_CameraFwdZ, camera_.cameraFwd[2]);
        parameters.Set(NVSDK_NGX_DLSSG_Parameter_CameraNear, camera_.cameraNear);
        parameters.Set(NVSDK_NGX_DLSSG_Parameter_CameraFar, camera_.cameraFar);
        parameters.Set(NVSDK_NGX_DLSSG_Parameter_CameraFOV, camera_.cameraFOV);
        parameters.Set(NVSDK_NGX_DLSSG_Parameter_CameraAspectRatio, camera_.cameraAspectRatio);
        parameters.Set(NVSDK_NGX_DLSSG_Parameter_DepthInverted, static_cast<unsigned>(camera_.depthInverted));
        parameters.Set(NVSDK_NGX_DLSSG_Parameter_ColorBuffersHDR, static_cast<unsigned>(camera_.colorBuffersHDR));
        parameters.Set(NVSDK_NGX_DLSSG_Parameter_CameraMotionIncluded, static_cast<unsigned>(camera_.cameraMotionIncluded));
        parameters.Set(NVSDK_NGX_DLSSG_Parameter_Reset, static_cast<unsigned>(camera_.reset));
        parameters.Set(NVSDK_NGX_DLSSG_Parameter_AutomodeOverrideReset, static_cast<unsigned>(camera_.automodeOverrideReset));
        parameters.Set(NVSDK_NGX_DLSSG_Parameter_NotRenderingGameFrames, static_cast<unsigned>(camera_.notRenderingGameFrames));
        parameters.Set(NVSDK_NGX_DLSSG_Parameter_OrthoProjection, static_cast<unsigned>(camera_.orthoProjection));
        parameters.Set(NVSDK_NGX_DLSSG_Parameter_MvecInvalidValue, camera_.motionVectorsInvalidValue);
        parameters.Set(NVSDK_NGX_DLSSG_Parameter_MvecDilated, static_cast<unsigned>(camera_.motionVectorsDilated));
    }
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_MvecJittered,
                   static_cast<unsigned>(frame.motion_vectors_jittered));
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_MenuDetectionEnabled, static_cast<unsigned>(camera_.menuDetectionEnabled));
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_MVecsSubrectWidth, config_.guide_width);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_MVecsSubrectHeight, config_.guide_height);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_MVecsSubrectBaseX, 0u);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_MVecsSubrectBaseY, 0u);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_DepthSubrectWidth, config_.guide_width);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_DepthSubrectHeight, config_.guide_height);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_DepthSubrectBaseX, 0u);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_DepthSubrectBaseY, 0u);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_InputBackbufferSubrectWidth, config_.width);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_InputBackbufferSubrectHeight, config_.height);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_InputBackbufferSubrectBaseX, 0u);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_InputBackbufferSubrectBaseY, 0u);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_OutputInterpolatedSubrectWidth, config_.width);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_OutputInterpolatedSubrectHeight, config_.height);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_OutputInterpolatedSubrectBaseX, 0u);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_OutputInterpolatedSubrectBaseY, 0u);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_HUDLessSubrectWidth, 0u);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_HUDLessSubrectHeight, 0u);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_HUDLessSubrectBaseX, 0u);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_HUDLessSubrectBaseY, 0u);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_UISubrectWidth, 0u);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_UISubrectHeight, 0u);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_UISubrectBaseX, 0u);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_UISubrectBaseY, 0u);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_UIAlphaSubrectWidth, 0u);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_UIAlphaSubrectHeight, 0u);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_UIAlphaSubrectBaseX, 0u);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_UIAlphaSubrectBaseY, 0u);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_BidirectionalDistortionFieldSubrectWidth, 0u);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_BidirectionalDistortionFieldSubrectHeight, 0u);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_BidirectionalDistortionFieldSubrectBaseX, 0u);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_BidirectionalDistortionFieldSubrectBaseY, 0u);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_BidirectionalDistortionField_LowPrecision_IsLowPrecision, 0u);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_BidirectionalDistortionField_LowPrecision_Bias, 0.0f);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_BidirectionalDistortionField_LowPrecision_Scale, 0.0f);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_OutputRealSubrectWidth, 0u);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_OutputRealSubrectHeight, 0u);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_OutputRealSubrectBaseX, 0u);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_OutputRealSubrectBaseY, 0u);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_MinRelativeLinearDepthObjectSeparation,
                   camera_.minRelativeLinearDepthObjectSeparation);

    for (unsigned index = 0; index < 3; ++index)
        transition(commands, bindings[index], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(commands, bindings[3], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    for (unsigned output = 0; output < frame.generated_count; ++output)
        transition(commands, frame.interpolated[output], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    NVSDK_NGX_Result status = NVSDK_NGX_Result_Success;
    for (unsigned output = 0; output < frame.generated_count; ++output)
    {
        parameters.Set(NVSDK_NGX_DLSSG_Parameter_OutputInterpolated,
                       frame.interpolated[output].resource);
        parameters.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, output + 1u);
        camera_.multiFrameIndex = output + 1u;
        const auto current = api_.evaluate(commands, handle_, parameters_, nullptr);
        if (NVSDK_NGX_FAILED(current))
        {
            status = current;
            break;
        }
    }
    for (unsigned output = 0; output < frame.generated_count; ++output)
    {
        D3D12_RESOURCE_BARRIER visibility{};
        visibility.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        visibility.UAV.pResource = frame.interpolated[output].resource;
        commands->ResourceBarrier(1, &visibility);
    }
    D3D12_RESOURCE_BARRIER hint_visibility{};
    hint_visibility.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    hint_visibility.UAV.pResource = bindings[3].resource;
    commands->ResourceBarrier(1, &hint_visibility);
    for (unsigned index = 0; index < 3; ++index)
        transition(commands, bindings[index], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, true);
    transition(commands, bindings[3], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
    for (unsigned output = 0; output < frame.generated_count; ++output)
        transition(commands, frame.interpolated[output], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
    recorded_ = true;
    submitted_ = false;
    evaluation_pending_ = true;
    evaluation_succeeded_ = !NVSDK_NGX_FAILED(status);
    pending_contract_ = frame.contract;
    if (evaluation_succeeded_) result = candidate;
    return status;
}

bool dlss_fg_stage::track_submission(ID3D12Fence *fence, std::uint64_t value)
{
    if (!recorded_ || fence == nullptr || value == 0 || !same_adapter(fence, config_.adapter_luid))
        return false;
    fence->AddRef();
    if (fence_ != nullptr) fence_->Release();
    fence_ = fence;
    fence_value_ = value;
    recorded_ = false;
    submitted_ = true;
    return true;
}

bool dlss_fg_stage::complete(std::uint64_t frame_id, bool succeeded)
{
    if (recorded_ || !submitted_ || !evaluation_pending_ || frame_id != pending_contract_.frame_id ||
        !fence_completed(fence_, fence_value_)) return false;
    history_.complete(pending_contract_, succeeded && evaluation_succeeded_);
    evaluation_pending_ = false;
    return true;
}

bool dlss_fg_stage::cancel_recording()
{
    if (submitted_) return false;
    recorded_ = false;
    evaluation_pending_ = false;
    evaluation_succeeded_ = false;
    history_.clear();
    return true;
}

bool dlss_fg_stage::release_completed()
{
    if (recorded_ || (submitted_ && !fence_completed(fence_, fence_value_)))
        return false;
    if (handle_ != nullptr)
    {
        if (NVSDK_NGX_FAILED(api_.release(handle_))) return false;
        handle_ = nullptr;
    }
    if (parameters_ != nullptr)
    {
        if (NVSDK_NGX_FAILED(api_.destroy(parameters_))) return false;
        parameters_ = nullptr;
    }
    if (fence_ != nullptr) { fence_->Release(); fence_ = nullptr; }
    fence_value_ = 0;
    submitted_ = false;
    evaluation_pending_ = false;
    history_.clear();
    return true;
}
}
