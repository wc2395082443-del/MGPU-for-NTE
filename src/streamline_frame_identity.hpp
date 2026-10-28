#pragma once

#include "motion_frame_packet.hpp"
#include "aligned_frame_policy.hpp"
#include "../ext/streamline/sl_core_types.h"
#include <cmath>
#include <cstring>
#include <windows.h>

namespace mgpu::sltags
{
inline motion_packet::identity read_identity(const void *frame, const void *viewport)
{
    motion_packet::identity result{};
    __try
    {
        const auto *handle = static_cast<const sl::ViewportHandle *>(viewport);
        if (handle != nullptr && handle->structType == sl::ViewportHandle::s_structType &&
            handle->structVersion >= 1)
        {
            result.viewport = static_cast<std::uint32_t>(*handle);
            result.viewport_known = true;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try
    {
        const auto *token = static_cast<const sl::FrameToken *>(frame);
        if (token != nullptr && token->structType == sl::FrameToken::s_structType &&
            token->structVersion >= 1)
        {
            result.frame = static_cast<std::uint32_t>(*token);
            result.frame_known = true;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return result;
}

inline aligned::guide_constants read_constants(const void *values, motion_packet::identity engine)
{
    aligned::guide_constants result{};
    result.key = {engine.frame, engine.viewport};
    if (!engine.frame_known || !engine.viewport_known || values == nullptr) return result;
    __try
    {
        const auto &input = *static_cast<const sl::Constants *>(values);
        if (input.structType != sl::Constants::s_structType || input.structVersion < 1 ||
            input.cameraMotionIncluded != sl::Boolean::eTrue ||
            input.motionVectors3D != sl::Boolean::eFalse ||
            input.motionVectorsJittered != sl::Boolean::eFalse ||
            input.depthInverted == sl::Boolean::eInvalid || input.reset == sl::Boolean::eInvalid ||
            !std::isfinite(input.mvecScale.x) || !std::isfinite(input.mvecScale.y) ||
            input.mvecScale.x == 0 || input.mvecScale.y == 0) return result;
        result.motion_scale_x = input.mvecScale.x;
        result.motion_scale_y = input.mvecScale.y;
        result.jitter_x = input.jitterOffset.x;
        result.jitter_y = input.jitterOffset.y;
        result.motion_dilated = input.motionVectorsDilated == sl::Boolean::eTrue;
        result.camera_near = input.cameraNear;
        result.camera_far = input.cameraFar;
        result.camera_fov = input.cameraFOV;
        static_assert(sizeof(input.clipToPrevClip) == sizeof(result.clip_to_previous));
        std::memcpy(result.clip_to_previous.data(), &input.clipToPrevClip, sizeof(input.clipToPrevClip));
        std::memcpy(result.previous_to_clip.data(), &input.prevClipToClip, sizeof(input.prevClipToClip));
        std::memcpy(result.clip_to_view.data(), &input.clipToCameraView, sizeof(input.clipToCameraView));
        const auto known = [](float value) { return std::isfinite(value) && value != sl::INVALID_FLOAT; };
        result.spatial_valid = known(result.jitter_x) && known(result.jitter_y) &&
            (input.motionVectorsDilated == sl::Boolean::eFalse || input.motionVectorsDilated == sl::Boolean::eTrue);
        for (unsigned index = 0; index != 16; ++index)
            result.spatial_valid = result.spatial_valid && known(result.clip_to_previous[index]) &&
                known(result.previous_to_clip[index]) && known(result.clip_to_view[index]);
        result.depth_inverted = input.depthInverted == sl::Boolean::eTrue;
        result.reset = input.reset == sl::Boolean::eTrue;
        result.valid = true;
        auto &camera = result.camera;
        std::memcpy(camera.view_to_clip.data(), &input.cameraViewToClip, sizeof(input.cameraViewToClip));
        std::memcpy(camera.clip_to_lens.data(), &input.clipToLensClip, sizeof(input.clipToLensClip));
        std::memcpy(camera.pinhole.data(), &input.cameraPinholeOffset, sizeof(input.cameraPinholeOffset));
        std::memcpy(camera.position.data(), &input.cameraPos, sizeof(input.cameraPos));
        std::memcpy(camera.up.data(), &input.cameraUp, sizeof(input.cameraUp));
        std::memcpy(camera.right.data(), &input.cameraRight, sizeof(input.cameraRight));
        std::memcpy(camera.forward.data(), &input.cameraFwd, sizeof(input.cameraFwd));
        camera.aspect = input.cameraAspectRatio;
        camera.invalid_motion = input.motionVectorsInvalidValue;
        if (input.structVersion >= 2 && known(input.minRelativeLinearDepthObjectSeparation))
            camera.min_depth_separation = input.minRelativeLinearDepthObjectSeparation;
        camera.valid = result.spatial_valid && input.orthographicProjection == sl::Boolean::eFalse &&
            known(result.camera_near) && known(result.camera_far) && known(result.camera_fov) &&
            known(camera.aspect) && result.camera_near > 0 && result.camera_far > result.camera_near &&
            result.camera_fov > 0 && result.camera_fov < 3.1415927f && camera.aspect > 0;
        bool lens_known = true;
        bool projection_nonzero = false;
        for (unsigned index = 0; index != 16; ++index)
        {
            camera.valid = camera.valid && known(camera.view_to_clip[index]);
            projection_nonzero = projection_nonzero || camera.view_to_clip[index] != 0.0f;
            lens_known = lens_known && known(camera.clip_to_lens[index]);
        }
        camera.valid = camera.valid && projection_nonzero;
        if (!lens_known)
        {
            camera.clip_to_lens = {};
            for (unsigned axis = 0; axis != 4; ++axis) camera.clip_to_lens[axis * 5] = 1.0f;
        }
        for (unsigned axis = 0; axis != 3; ++axis)
            camera.valid = camera.valid && known(camera.position[axis]) && known(camera.up[axis]) &&
                known(camera.right[axis]) && known(camera.forward[axis]);
        for (float &offset : camera.pinhole) if (!known(offset)) offset = 0.0f;
        LARGE_INTEGER ticks{}, frequency{};
        if (camera.valid && QueryPerformanceCounter(&ticks) && QueryPerformanceFrequency(&frequency) &&
            ticks.QuadPart > 0 && frequency.QuadPart > 0)
            camera.capture_ns = static_cast<std::uint64_t>(
                double(ticks.QuadPart) * 1000000000.0 / double(frequency.QuadPart));
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return result;
}
}
